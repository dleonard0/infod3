#pragma once

#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rxbuf.h"

#define ARRAY_SIZE(a) (sizeof (a) / sizeof (a)[0])

struct proto {
	int mode;
	void *udata;
	void (*udata_free)(void *udata);
	int (*on_input)(struct proto *p, unsigned char msg,
			 const char *data, unsigned int datalen);
	int (*on_sendv)(struct proto *p, const struct iovec *, int);
	void (*on_error)(struct proto *p, const char *msg);

#ifndef SMALL
	struct rxbuf rx;		/* used by text and binary */
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
#endif

};

#define proto_errorv	_protopriv_proto_errorv
#define recv_framed	_protopriv_recv_framed
#define output_framed	_protopriv_output_framed
#define recv_binary	_protopriv_recv_binary
#define output_binary	_protopriv_output_binary
#define recv_text	_protopriv_recv_text
#define output_text	_protopriv_output_text
#define to_binary_iov	_protopriv_to_binary_iov
#define output_binary_error _protopriv_output_binary_error

void proto_errorv(struct proto *p, int err, const char *tag,
        const char *fmt, va_list ap);

int recv_framed(struct proto *p, const char *net, unsigned int netlen);
int output_framed(struct proto *p, unsigned char msg, const char *fmt,
	va_list ap);
__attribute__((format(printf, 3, 4)))
int output_binary_error(struct proto *p, int err, const char *fmt, ...);
int to_binary_iov(struct proto *p, struct iovec *iov, int maxiov,
        char *work, size_t worksz, const char *fmt, va_list ap);

#ifndef SMALL
int recv_binary(struct proto *p, const char *net, unsigned int netlen);
int output_binary(struct proto *p, unsigned char msg, const char *fmt,
	va_list ap);
int recv_text(struct proto *p, const char *net, unsigned int netlen);
int output_text(struct proto *p, unsigned char msg, const char *fmt,
	va_list ap);
#endif
