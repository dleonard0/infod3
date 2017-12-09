#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/uio.h>

#include "proto.h"
#include "protopriv.h"

/* Common function for sending a message to on_error */
void
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
#ifndef SMALL
	p->rx.buf = NULL;
	p->rx.max = p->rx.len = 0;
	p->t.state = T_BOL;
#endif
	return p;
}

void
proto_free(struct proto *p)
{
	if (!p)
		return;
	proto_set_udata(p, NULL, NULL);
#ifndef SMALL
	free(p->rx.buf);
#endif
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
int
proto_output_error(struct proto *p, const char *fmt, ...)
{
	va_list ap;
	char buf[16384];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	proto_output(p, MSG_ERROR, "%s", buf);
	return -1;
}

static int
recv_error(struct proto *p, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	proto_errorv(p, err, "proto_recv", fmt, ap);
	va_end(ap);
	return -1;
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

	if (net[netlen])
		return recv_error(p, EINVAL, "terminal NUL missing");

#ifndef SMALL
	/* Autodetect text/binary mode based on first byte */
	if (p->mode == PROTO_MODE_UNKNOWN) {
		char ch = net[0];
		if (ch == '\n' || ch == '\r' || ch == ' ' ||
		    (ch >= 0x40 && ch <= '~'))
			p->mode = PROTO_MODE_TEXT;
		else
			p->mode = PROTO_MODE_BINARY;
	}
#endif

	switch (p->mode) {
#ifndef SMALL
	case PROTO_MODE_BINARY:
		return recv_binary(p, net, netlen);
	case PROTO_MODE_TEXT:
		return recv_text(p, net, netlen);
#endif
	case PROTO_MODE_FRAMED:
		return recv_framed(p, net, netlen);
	default:
		errno = EINVAL; /* bad mode */
		return -1;
	}
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
proto_outputv(struct proto *p, unsigned char msg, const char *fmt,
	va_list ap)
{
	if (p->mode == PROTO_MODE_UNKNOWN)
		p->mode = PROTO_MODE_BINARY; /* prefer binary */

	switch (p->mode) {
#ifndef SMALL
	case  PROTO_MODE_BINARY:
		return output_binary(p, msg, fmt, ap);
	case PROTO_MODE_TEXT:
		return output_text(p, msg, fmt, ap);
#endif
	case PROTO_MODE_FRAMED:
		return output_framed(p, msg, fmt, ap);
	default:
		return output_error(p, EINVAL, "bad mode %d", p->mode);
	}
}

int
proto_output(struct proto *p, unsigned char msg, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = proto_outputv(p, msg, fmt, ap);
	va_end(ap);

	return ret;
}
