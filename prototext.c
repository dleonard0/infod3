#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include <sys/uio.h>

#include "proto.h"
#include "protopriv.h"

#ifndef SMALL

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
				goto again;
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
			goto again;
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
				int ret;
				/* pass up full input command */
				if (rxbuf_zeropad(&p->rx) == -1)
					return -1;
				ret = p->on_input(p, p->rx.buf[0] & 0xff,
					&p->rx.buf[1], p->rx.len - 1);
				if (ret <= 0)
					return ret;
			}
			t->state = T_BOL;
		} else if (!*t->fmt) {
			proto_output_error(p, "unexpected arg for '%s'", t->cmd);
			t->state = T_ERROR;
			goto again;
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
				goto again;
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

int
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
	proto_errorv(p, err, "proto_output() text", fmt, ap);
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
			"string too big, len %u > %u", len, 0xffff);
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

int
output_text(struct proto *p, unsigned char msg, const char *fmt, va_list ap)
{
	unsigned int j;
	const char *word;
	const char *tfmt;
	int optional;
	char ibuf[4];
	int ch;
	int len;
	const char *str;
	char *nulpos;
	int star;
	const char *ofmt = fmt;

	outbuf_init(p);

	/* Find the command/message structure */
	for (j = 0; cmdtab[j].word; j++)
		if (cmdtab[j].id == msg)
			break;
	word = cmdtab[j].word;
	if (!word)
		return output_text_error(p, EINVAL,
			"unknown msg 0x%02x", msg);

	/* Send the command word first */
	if (proto_outbuf(p, word, strlen(word)) == -1)
		return -1;

	tfmt = cmdtab[j].fmt;
	optional = 0;
	while (*fmt) {
		char f, t;

		f = *fmt++;
		if (f == ' ')
			continue;
		if (f != '%')
			return output_text_error(p, EINVAL,
				"%s/%s: unexpected '%c' in fmt",
				word, ofmt, f);
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
				"%s/%s: can't match %%%c against tfmt '%s'",
				word, ofmt, f, cmdtab[j].fmt);
		case 'i':
			if (f != 'c')
				return output_text_error(p, EINVAL,
					"%s/%s: expected %%c not %%%c for '%s'",
					word, ofmt, f, cmdtab[j].fmt);
			ch = va_arg(ap, int);
			len = snprintf(ibuf, sizeof ibuf, "%u", ch & 0xff);
			if (proto_outbuf(p, ibuf, len) == -1)
				return -1;
			break;
		case '0':
			if (f != 'c')
				return output_text_error(p, EINVAL,
					"%s/%s: expected %%c not %%%c for '%s'",
					word, ofmt, f, cmdtab[j].fmt);
			ch = va_arg(ap, int);
			if (ch != 0)
				return output_text_error(p, EINVAL,
					"%s/%s: expected 0 for %%c, got %d",
					word, ofmt, ch);
			/* no need to emit anything here */
			break;
		case 't':
			if ((star = (f == '*')))
				f = *fmt++;
			if (f != 's')
				return output_text_error(p, EINVAL,
					"%s/%s: expected %%s not %%%c for '%s'",
					word, ofmt, f, cmdtab[j].fmt);

			if (star) {
				len = va_arg(ap, int);
				str = va_arg(ap, char *);
			} else {
				str = va_arg(ap, char *);
				len = strlen(str);
				if (output_text_string(p, str, len) == -1)
					return -1;
				break;
			}

			while (*fmt == ' ')
				fmt++;

			/* The sequence '...t|0t' can be satisfied by %*s,
			 * but only when the data contains a NUL */
			if (star && strcmp(tfmt, "|0t") == 0 &&
			    !*fmt && (nulpos = memchr(str, 0, len)))
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
			"%s/%s: missing arguments for '%s'",
			word, ofmt, cmdtab[j].fmt);
	if (proto_outbuf(p, "\r\n", 2) == -1)
		return -1;
	return outbuf_flush(p);
}

#endif /* !SMALL */
