#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include <sys/uio.h>

#include "proto.h"

#define SIZECHUNK 1024		/* The increment that rxbuf grows by */
#define ARRAY_SIZE(a) (sizeof (a) / sizeof (a)[0])

struct proto {
	int mode;
	void *udata;
	void (*udata_free)(void *udata);
	int (*on_input)(struct proto *p, unsigned char msg,
			 const char *data, unsigned int datalen);
	int (*on_sendv)(struct proto *p, const struct iovec *, int);
	void (*on_error)(struct proto *p, const char *msg);

	/* Receive state */
	struct rxbuf {
		char *buf;
		size_t max;
		size_t len;
	} rx;

	struct textproto {
		enum tstate {
			T_ERROR,	/* discard until EOL */
			T_BOL,		/* skip space until cmd */
			T_CMD,		/* consume command word */
			T_ARGSP,	/* skip space until arg */
			T_INT,		/* consume decimal integer */
			T_STRBEG,	/* expect " or unquoted string */
			T_STR,		/* consume unquoted string */
			T_QSTR,		/* consume quoted string */
			T_QOCT,		/* consume \ooo octal code */
		} state;
		const char *fmt;
		uint16_t intval;
		char cmd[16];
		unsigned char cmdlen;
		unsigned char counter;
		unsigned char optional;
	} t;
};

/* Common function for sending a message to on_error */
static void
proto_errorv(struct proto *p, int err, const char *tag,
	const char *fmt, va_list ap)
{
	if (p->on_error) {
		char buf[16384];
		int n = 0;
		if (tag)
			n = snprintf(buf, sizeof buf, "%s: ", tag);
		vsnprintf(buf + n, sizeof buf - n, fmt, ap);
		p->on_error(p, buf);
	} else {
		fprintf(stderr, "%s: ", tag);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	}
	errno = err;
}

/* -- rx buffer management -- */

static size_t
roundup(size_t n, size_t align)
{
	size_t remainder = n % align;
	return remainder ? n + (align - remainder) : n;
}

/* Resize the rxbuf in increments of SIZECHUNK */
static int
rxbuf_resize(struct rxbuf *rx, size_t sz)
{
	char *newbuf;

	if (sz >= 0x10000) {
		errno = ENOSPC;
		return -1;
	}
	/* Round up sz to next SIZECHUNK */
	sz = roundup(sz, SIZECHUNK);
	assert(sz >= rx->len);
	if (rx->max == sz)
		return 0;

	newbuf = realloc(rx->buf, sz);
	if (!newbuf)
		return -1;
	rx->max = sz;
	rx->buf = newbuf;
	return 0;
}

static int
rxbuf_addc(struct rxbuf *rx, char ch)
{
	if ((rx->len >= rx->max) && rxbuf_resize(rx, rx->len + 1) == -1)
		return -1;
	rx->buf[rx->len++] = ch;
	return 0;
}

static int
rxbuf_add(struct rxbuf *rx, const void *p, size_t n)
{
	if (rxbuf_resize(rx, rx->len + n) == -1)
		return -1;
	memcpy(rx->buf + rx->len, p, n);
	rx->len += n;
	return 0;
}

static void
rxbuf_init(struct rxbuf *rx)
{
	rx->max = 0;
	rx->len = 0;
	rx->buf = NULL;
}

/* Clears the buffer, and ensures it has space for sz chars */
static int
rxbuf_clear(struct rxbuf *rx, size_t sz)
{
	rx->len = 0;
	return rxbuf_resize(rx, sz);
}

static void
rxbuf_free(struct rxbuf *rx)
{
	free(rx->buf);
}

static void
rxbuf_trimspace(struct rxbuf *rx)
{
	while (rx->len && rx->buf[rx->len - 1] == ' ')
		--rx->len;
}

/* -- proto init, fini, setters and getters -- */

struct proto *
proto_new()
{
	struct proto *p = malloc(sizeof *p);
	if (!p)
		return NULL;
	p->mode = PROTO_MODE_UNKNOWN;
	p->udata = NULL;
	p->udata_free = NULL;
	p->on_input = NULL;
	rxbuf_init(&p->rx);
	p->t.state = T_BOL;
	return p;
}

void
proto_free(struct proto *p)
{
	if (!p)
		return;
	proto_set_udata(p, NULL, NULL);
	rxbuf_free(&p->rx);
	free(p);
}

void
proto_set_udata(struct proto *p, void *udata, void (*udata_free)(void *))
{
	if (p->udata_free)
		p->udata_free(p->udata);
	p->udata = udata;
	p->udata_free = udata_free;
}

void *
proto_get_udata(struct proto *p)
{
	return p->udata;
}

void
proto_set_on_input(struct proto *p,
	int (*on_input)(struct proto *p, unsigned char msg,
			 const char *data, unsigned int datalen))
{
	p->on_input = on_input;
}

void
proto_set_on_sendv(struct proto *p,
	int (*on_sendv)(struct proto *p, const struct iovec *, int))
{
	p->on_sendv = on_sendv;
}

void
proto_set_on_error(struct proto *p,
	void (*on_error)(struct proto *p, const char *msg))
{
	p->on_error = on_error;
}

int
proto_set_mode(struct proto *p, int mode)
{
	p->mode = mode;
	return 0;
}

int
proto_get_mode(struct proto *p)
{
	return p->mode;
}

/* -- error handling -- */

/*
 * Sends a MSG_ERROR to the peer.
 * This is automatically called from the proto_recv() path on protocol errors.
 * Returns -1.
 */
static int
proto_output_error(struct proto *p, const char *fmt, ...)
{
	va_list ap;
	char buf[16384];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	proto_output(p, "%c%s", MSG_ERROR, buf);
	return -1;
}

/* -- binary decode -- */

/* Returns the length field (bytes 1 and 2) from the buffer,
 * or returns ~0 if not available */
static uint16_t
binary_pkt_len(struct rxbuf *rx)
{
	if (rx->len < 3)
		return ~0;
	return (rx->buf[1] & 0xff) << 8 |
	       (rx->buf[2] & 0xff) << 0;
}

static int
recv_binary(struct proto *p, const char *net, unsigned int netlen)
{
	int ret = 0;

	if (!netlen) {
		if (p->on_input)
			ret =p->on_input(p, MSG_EOF, NULL, 0);
		return ret;
	}

	while (netlen) {
		unsigned int take;
		uint16_t sz = binary_pkt_len(&p->rx);

		if (p->rx.len < 3) {
			if (rxbuf_resize(&p->rx, 3) == -1)
				return -1;
			take = 3 - p->rx.len;
		} else {
			take = 3 + sz - p->rx.len;
			if (rxbuf_resize(&p->rx, 3 + sz) == -1)
				return -1;
		}

		if (take > netlen)
			take = netlen;
		rxbuf_add(&p->rx, net, take);
		net += take;
		netlen -= take;
		ret += take;

		if (p->rx.len >= 3 &&
		    p->rx.len == 3 + (sz = binary_pkt_len(&p->rx)))
		{
			if (p->on_input) {
				int n = p->on_input(p, p->rx.buf[0] & 0xff,
					p->rx.buf + 3, sz);
				if (n <= 0)
					return n;
				ret += n;
			}
			if (rxbuf_clear(&p->rx, 3) == -1)
				return -1;
		}
	}
	return ret;
}

/* -- framed decode -- */

static int
recv_framed(struct proto *p, const char *net, unsigned int netlen)
{
	if (!p->on_input)
		return netlen;
	if (!netlen)
		return p->on_input(p, MSG_EOF, NULL, 0);
	return p->on_input(p, *net, net + 1, netlen - 1);
}

/* -- text decode -- */

/* Command (and message) table */
static struct {
	const char *word;
	unsigned char id;
	const char *fmt;
} cmdtab[] = {
	{ "HELLO", CMD_HELLO, "i|t" },
	{ "SUB", CMD_SUB, "t" },
	{ "UNSUB", CMD_UNSUB, "t" },
	{ "GET", CMD_GET, "t" },
	{ "PUT", CMD_PUT, "t|0t" },
	{ "BEGIN", CMD_BEGIN, "" },
	{ "COMMIT", CMD_COMMIT, "" },
	{ "PING", CMD_PING, "|t" },
	{ "VERSION", MSG_VERSION, "i|t" },
	{ "INFO", MSG_INFO, "t|0t" },
	{ "PONG", MSG_PONG, "|t" },
	{ "ERROR", MSG_ERROR, "t" },
	{}
};

/* Decode one byte from the text protocol.
 * Returns -1 on unrecoverable errors.
 * Returns 1 on good data and on protocol errors. */
static int
recv_text_1ch(struct proto *p, char ch)
{
	unsigned int i;
	struct textproto *t = &p->t;

	/*
	 * The text command line is decoded into the p->rx buffer:
	 * The first byte of the buffer will be the decoded command ID,
	 * and the rest of the buffer will be packed argument data.
	 */

again:
	switch (t->state) {
	case T_ERROR:
		/* consume anything till <cr> or <lf> */
		if (ch != '\n' && ch != '\r')
			break;
		t->state = T_BOL; /* falthru */
	case T_BOL:
		/* (normal entry point) */
		/* skip initial <sp> and blank lines */
		if (ch == ' ' || ch == '\n' || ch == '\r')
			break;
		t->cmdlen = 0;
		t->state = T_CMD; /* fallthru */
	case T_CMD:
		if (ch != ' ' && ch != '\n' && ch != '\r') {
			/* accumulate command word characters */
			t->cmd[t->cmdlen++] = ch;
			if (t->cmdlen >= sizeof t->cmd) {
				proto_output_error(p, "long command");
				t->state = T_ERROR;
			}
			break;
		}
		/* end of command word; look it up */
		t->cmd[t->cmdlen] = '\0';
		for (i = 0; cmdtab[i].word; i++)
			if (strcasecmp(cmdtab[i].word, t->cmd) == 0)
				break;
		if (!cmdtab[i].word) {
			proto_output_error(p, "unknown command '%s'", t->cmd);
			t->state = T_ERROR;
			break;
		}
		/* store command ID at front of rxbuf */
		if (rxbuf_clear(&p->rx, 1) == -1)
			return -1;
		if (rxbuf_addc(&p->rx, cmdtab[i].id) == -1)
			return -1;
		t->fmt = cmdtab[i].fmt;
		t->optional = 0;
		t->state = T_ARGSP; /* fallthru */
	case T_ARGSP:
		/* skip spaces before next argument */
		if (ch == ' ')
			break;
		if (*t->fmt == '|') { /* future args are optional? */
			t->fmt++;
			t->optional = 1;
		}
		if (ch == '\n' || ch == '\r') {
			/* end of command line */
			if (!t->optional && *t->fmt) {
				proto_output_error(p, "missing arg for '%s'", t->cmd);
			} else if (p->on_input) {
				/* pass up full input command */
				int ret = p->on_input(p, p->rx.buf[0] & 0xff,
					&p->rx.buf[1], p->rx.len - 1);
				if (ret <= 0)
					return ret;
			}
			t->state = T_BOL;
		} else if (!*t->fmt) {
			proto_output_error(p, "unexpected arg for '%s'", t->cmd);
			t->state = T_ERROR;
		} else {
			/* At this point all leading space has been skipped,
			 * so we jump to the next state immediately */
			switch (*t->fmt++) {
			case 'i': t->state = T_INT;
				  t->intval = 0;
				  goto again;
			case 't': t->state = T_STRBEG;
				  goto again;
			case '0': if (rxbuf_addc(&p->rx, '\0') == -1)
					t->state = T_ERROR;
				  goto again;
			default:  abort();
			}
		}
		break;
	case T_INT:
		/* consume decimal <int> and store as a byte */
		if (ch >= '0' && ch <= '9') {
			t->intval = t->intval * 10 + ch - '0';
			if (t->intval > 255) {
				proto_output_error(p, "integer overflow");
				t->state = T_ERROR;
			}
			break;
		}
		if (rxbuf_addc(&p->rx, t->intval) == -1)
			return -1;
		t->state = T_ARGSP;
		goto again;
	case T_STRBEG:
		/* beginning of string, possibly quoted */
		if (ch == '"') {
			t->state = T_QSTR;
			break;
		}
		t->state = T_STR; /* fallthru */
	case T_STR:
		/* unquoted string */
		if (ch == '\r' || ch == '\n' || (*t->fmt && ch == ' ')) {
			/* Special case when there are no further args:
			 * allow unquoted space characters, but trim
			 * any trailing spaces when we see \n or \r */
			if (!*t->fmt)
				rxbuf_trimspace(&p->rx);
			t->state = T_ARGSP;
			goto again;
		}
		if (rxbuf_addc(&p->rx, ch) == -1)
			return -1;
		break;
	case T_QSTR:
		/* consuming quoted string */
		if (ch == '\r' || ch == '\n') {
			proto_output_error(p, "unclosed \"");
			t->state = T_BOL;
		} else if (ch == '\\') {
			t->counter = 3;
			t->intval = 0;
			t->state = T_QOCT;
		} else if (ch == '"') {
			t->state = T_ARGSP;
		} else {
			if (rxbuf_addc(&p->rx, ch) == -1)
				return -1;
		}
		break;
	case T_QOCT:
		/* consuming quoted octal escape */
		if (ch < '0' || ch > '7') {
			proto_output_error(p, "expected octal after backslash");
			t->state = T_ERROR;
			goto again;
		}
		t->intval = (t->intval << 3) | (ch - '0');
		if (!--t->counter) {
			/* XXX check for \000 ? */
			if (rxbuf_addc(&p->rx, t->intval) == -1)
				return -1;
			t->state = T_QSTR;
		}
		break;
	}
	return 1;
}

static int
recv_text(struct proto *p, const char *net, unsigned int netlen)
{
	unsigned int i;
	int ret = 0;

	if (!netlen) {
		if (recv_text_1ch(p, '\n') == -1)
			return -1;
		if (p->on_input)
			ret = p->on_input(p, MSG_EOF, NULL, 0);
		return ret;
	}

	for (i = 0; i < netlen; i++) {
		int n = recv_text_1ch(p, net[i]);
		if (n <= 0)
			return n;
		ret += n;
	}
	return ret;
}


/* A partial receive from the network */
int
proto_recv(struct proto *p, const void *netv, unsigned int netlen)
{
	const char *net = netv;

	if (netlen == 0) {
		/* Handle a close message */
		if (p->on_input)
			p->on_input(p, MSG_EOF, NULL, 0);
		return 0;
	}

	if (p->mode == PROTO_MODE_UNKNOWN) {
		/* Select mode based on first byte */
		char ch = net[0];
		if (ch == '\n' || ch == '\r' || ch == ' ' ||
		    (ch >= 0x40 && ch <= '~'))
			p->mode = PROTO_MODE_TEXT;
		else
			p->mode = PROTO_MODE_BINARY;
	}

	switch (p->mode) {
	case PROTO_MODE_BINARY:
		return recv_binary(p, net, netlen);
	case PROTO_MODE_TEXT:
		return recv_text(p, net, netlen);
	case PROTO_MODE_FRAMED:
		return recv_framed(p, net, netlen);
	default:
		errno = EINVAL; /* bad mode */
		return -1;
	}
}

/* -- text encode -- */

/* An output buffer for text proto composition.
 * Because output by the text encoder is entirely handled
 * in one call to proto_output(), we can use a shared output
 * buffer to save space. The buffer is used to avoid many independent
 * writev() calls as the output line is constructed. */
static char outbuf[4096];
unsigned int outbuf_len;

static void
outbuf_init(struct proto *p)
{
	outbuf_len = 0;
}

static int
outbuf_flush(struct proto *p)
{
	if (outbuf_len) {
		struct iovec iov;
		iov.iov_base = outbuf;
		iov.iov_len = outbuf_len;
		outbuf_len = 0;
		if (p->on_sendv)
			return p->on_sendv(p, &iov, 1);
	}
	return 0;
}

static int
proto_outbuf(struct proto *p, const char *data, unsigned int datasz)
{
	unsigned int space = sizeof sizeof outbuf - outbuf_len;
	if (datasz <= space) {
		/* Small writes: append to buffer */
		memcpy(&outbuf[outbuf_len], data, datasz);
		outbuf_len += datasz;
	} else if (datasz >= sizeof outbuf) {
		/* Huge writes: send buffer current data immediately */
		struct iovec iov[2];
		iov[0].iov_base = outbuf;
		iov[0].iov_len = outbuf_len;
		iov[1].iov_base = (void *)data;
		iov[1].iov_len = datasz;
		outbuf_len = 0;
		if (p->on_sendv)
			return p->on_sendv(p, iov, 2);
	} else /* (datasz > space && datasz < sizeof outbuf) */ {
		/* Medium writes: pack buffer, drain, start new fill */
		memcpy(&outbuf[outbuf_len], data, space);
		outbuf_len += space;
		if (outbuf_flush(p) == -1)
			return -1;
		outbuf_len = datasz - space;
		memcpy(outbuf, data + space, outbuf_len);
	}
	return 0;
}

static int
outbuf_putc(struct proto *p, int ch)
{
	outbuf[outbuf_len++] = ch;
	if (outbuf_len == sizeof outbuf)
		return outbuf_flush(p);
	return 0;
}

/* Called when the output has been aborted */
static int
outbuf_cancel(struct proto *p)
{
	return 0;
}

/*
 * Raises a local error produced from the proto_output() path in text mode.
 * Prints an error message on stderr.
 * Cancels the current text output buffer.
 * Sets errno.
 * Returns -1.
 */
static int
output_text_error(struct proto *p, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	proto_errorv(p, err, "proto_output{text}", fmt, ap);
	va_end(ap);
	(void)outbuf_cancel(p);
	return -1;
}

/* Sends a string argument; quoting if needed */
static int
output_text_string(struct proto *p, const char *str, unsigned int len)
{
	if (len > 0xffff)
		return output_text_error(p, EINVAL,
			"string too big, %u > %u", len, 0xffff);
	if (len == 0 ||
	    str[0] == '"' ||
	    memchr(str, ' ', len) ||
	    memchr(str, '\r', len) ||
	    memchr(str, '\n', len))
	{
		if (outbuf_putc(p, '"') == -1)
			return -1;
		while (len--) {
			char ch = *str++;
			if (ch == '\n' || ch == '\r' ||
			    ch == '"' || ch == '\\')
			{
				char esc[4] = { '\\',
						'0' | ((ch >> 6) & 7),
						'0' | ((ch >> 3) & 7),
						'0' | ((ch >> 0) & 7) };
				if (proto_outbuf(p, esc, sizeof esc) == -1)
					return -1;
			} else {
				if (outbuf_putc(p, ch) == -1)
					return -1;
			}
		}
		return outbuf_putc(p, '"');
	}

	return proto_outbuf(p, str, len);
}

static int
output_text(struct proto *p, const char *fmt, va_list ap)
{
	unsigned char msgid;
	unsigned int j;
	const char *word;
	const char *tfmt;
	int optional;
	char ibuf[4];
	int ch;
	int len;
	const char *str;
	char *nulpos;

	outbuf_init(p);

	/* The format string must start with %c for the msg ID */
	while (*fmt == ' ') fmt++;
	if (fmt[0] != '%' || fmt[1] != 'c')
		return output_text_error(p, EINVAL,
			"format string must start with %%c (%s)", fmt);
	fmt += 2;
	msgid = va_arg(ap, int);

	/* Find the command/message structure */
	for (j = 0; cmdtab[j].word; j++)
		if (cmdtab[j].id == msgid)
			break;
	word = cmdtab[j].word;
	if (!word)
		return output_text_error(p, EINVAL,
			"unknown command ID %02x", msgid);

	/* Send the command word first */
	if (proto_outbuf(p, word, strlen(word)) == -1)
		return -1;

	tfmt = cmdtab[j].fmt;
	optional = 0;
	while (*fmt) {
		char f, t;

		f = *fmt++;
		if (f == ' ') continue;
		if (f != '%')
			return output_text_error(p, EINVAL,
				"%s: unexpected char in format: '%c'",
				word,f);
		f = *fmt++;

		t = *tfmt++;
		while (t == '|') {
			optional = 1;
			t = *tfmt++;
		}

		/* Send a space before each arg, but not %c,0 */
		if (t != '0')
			if (outbuf_putc(p, ' ') == -1)
				return -1;

		switch (t) {
		case '\0':
			return output_text_error(p, EINVAL,
				"%s: excess format %%%c can't be matched",
				word, f);
		case 'i':
			if (f != 'c')
				return output_text_error(p, EINVAL,
					"%s: expected %%c, got %%%c", word, f);
			ch = va_arg(ap, int);
			len = snprintf(ibuf, sizeof ibuf, "%u", ch & 0xff);
			if (proto_outbuf(p, ibuf, len) == -1)
				return -1;
			break;
		case '0':
			if (f != 'c')
				return output_text_error(p, EINVAL,
					"%s: expected %%c, got %%%c", word, f);
			ch = va_arg(ap, int);
			if (ch != 0)
				return output_text_error(p, EINVAL,
					"%s: expected 0 for %%c, got %d",
					word, ch);
			/* no need to emit anything here */
			break;
		case 't':
			if (f == 's') { /* got %s */
				str = va_arg(ap, char *);
				if (!str)
					return output_text_error(p,
						EINVAL, "%s: got NULL for %%s",
						word);
				len = strlen(str);
				if (output_text_string(p, str, len) == -1)
					return -1;
				break;
			}
			if (f != '*')
				return output_text_error(p, EINVAL,
					"%s: unexpected %%%c for text",
					word, f);
			f = *fmt++;
			if (f != 's')
				return output_text_error(p, EINVAL,
					"%s: unexpected %%*%c for text",
					word, f);
			len = va_arg(ap, int);
			str = va_arg(ap, char *);
			/* The sequence '...t|0t' can be satisfied by %*s,
			 * but only when the data contains a NUL */
			if (strcmp(tfmt, "|0t") == 0 &&
			    !*fmt &&
			    (nulpos = memchr(str, 0, len)))
			{
				if (output_text_string(p, str,
				    nulpos - str) == -1)
					return -1;
				if (outbuf_putc(p, ' ') == -1)
					return -1;
				len -= (nulpos + 1) - str;
				str = nulpos + 1;
				tfmt = ""; /* skip |0t */
				optional = 1;
			}
			if (output_text_string(p, str, len)
				== -1) return -1;
			break;
		default: abort();
		}
	}
	if (!optional && *tfmt && *tfmt != '|')
		return output_text_error(p, EINVAL,
			"%s: missing required argument (%c)", word, *tfmt);
	if (proto_outbuf(p, "\r\n", 2) == -1)
		return -1;
	return outbuf_flush(p);
}

/* -- binary encode -- */

/*
 * Raises a local error produced from the proto_output() path in binary mode.
 * Prints an error message on stderr.
 * Sets errno.
 * Returns -1.
 */
static int
output_binary_error(struct proto *p, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	proto_errorv(p, err, "proto_output{binary}", fmt, ap);
	va_end(ap);
	return -1;
}

/* Convert the format string into an iov list.
 * The format string is quite restricted, so it is heavily tested.
 * Some iovs may point to static memory that will be overwritten next time.
 * Returns number if iov, or -1 on error. */
static int
to_binary_iov(struct proto *p, struct iovec *iov, int maxiov,
	const char *fmt, va_list ap)
{
	struct iovec *iov_start = iov;
	char *str;
	char f;
	static char buf[5];
	int buflen = 0;
	char *lenp = NULL;

	while ((f = *fmt++)) {
		if (f == ' ')
			continue;
		if (f != '%')
			return output_binary_error(p, EINVAL,
				"unexpected format char %c", f);
		if (iov >= iov_start + maxiov) {
			errno = ENOMEM;
			return -1;	/* too many % args */
		}
		if (iov == iov_start && *fmt != 'c')
			return output_binary_error(p, EINVAL,
				"expected %%c but got %%%c", *fmt);
		switch (*fmt++) {
		case '*':
			if (*fmt++ != 's') abort();
			iov->iov_len = va_arg(ap, int);
			iov->iov_base = va_arg(ap, char *);
			iov++;
			break;
		case 's':
			str = va_arg(ap, char *);
			iov->iov_len = strlen(str);
			iov->iov_base = (void *)str;
			iov++;
			break;
		case 'c':
			if (buflen >= sizeof(buf)) {
				errno = ENOMEM;
				return -1; /* too many %c */
			}
			buf[buflen] = va_arg(ap, int) & 0xff; /* promoted */
			iov->iov_base = &buf[buflen];
			if (iov == iov_start) {
				/* Reserve space for the length bytes */
				iov->iov_len = 3;
				lenp = &buf[buflen + 1];
				buflen += 3;
			} else {
				iov->iov_len = 1;
				buflen++;
			}
			iov++;
			break;
		default:
			return output_binary_error(p, EINVAL,
				"unknown format %%%c", *(fmt - 1));
		}
	}
	if (iov == iov_start)
		return output_binary_error(p, EINVAL, "empty format");
	if (lenp) {
		/* Calculate the total size */
		size_t len = 0;
		struct iovec *i;
		// assert(iov_start->iov_len == 3);
		for (i = iov_start + 1; i < iov; i++)
			len += i->iov_len;
		if (len > 0xffff) {
			errno = ENOMEM;
			return -1;
		}
		lenp[0] = (len >> 8) & 0xff;
		lenp[1] = (len >> 0) & 0xff;
	}

	return iov - iov_start;
}

static int
output_binary(struct proto *p, const char *fmt, va_list ap)
{
	struct iovec iov[10];
	int niov = to_binary_iov(p, iov, 10, fmt, ap);
	if (niov < 0)
		return -1;
	if (p->on_sendv)
		return p->on_sendv(p, iov, niov);
	return 0;
}

static int
output_framed(struct proto *p, const char *fmt, va_list ap)
{
	struct iovec iov[10];
	int niov = to_binary_iov(p, iov, 10, fmt, ap);
	if (niov < 0)
		return -1;
	/* Framed is just like binary except that we omit the
	 * two length bytes. We do that by patching iov[0] */
	iov[0].iov_len = 1;
	if (p->on_sendv)
		return p->on_sendv(p, iov, niov);
	return 0;
}

static int
output_error(struct proto *p, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	proto_errorv(p, err, "proto_output", fmt, ap);
	va_end(ap);
	return -1;
}

int
proto_output(struct proto *p, const char *fmt, ...)
{
	va_list ap;
	int ret;

	if (p->mode == PROTO_MODE_UNKNOWN)
		p->mode = PROTO_MODE_BINARY; /* prefer binary */

	va_start(ap, fmt);
	switch (p->mode) {
	case  PROTO_MODE_BINARY:
		ret = output_binary(p, fmt, ap);
		break;
	case PROTO_MODE_TEXT:
		ret = output_text(p, fmt, ap);
		break;
	case PROTO_MODE_FRAMED:
		ret = output_framed(p, fmt, ap);
		break;
	default:
		ret = output_error(p, EINVAL, "bad mode %d", p->mode);
	}
	va_end(ap);
	return ret;
}
