#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/uio.h>

#include "proto.h"

#define DEBUG 0
#if DEBUG
# define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
# define dprintf(...) /* nothing */
#endif

#define cHILITE "\033[31;1m"
#define cGOOD "\033[32;1m"
#define cNORMAL "\033[m"
#define FAILED  cHILITE "failed" cNORMAL
#define PASSED  cGOOD "passed" cNORMAL

static struct {
	unsigned int counter;
	/* Parameters received by mock_on_input_fn() */
	struct proto *p;
	unsigned char msg;
	char data[65536 + 4];
	int datalen;

	/* What mock_on_input_fn() should return when called */
	int retval;
} mock_on_input;

static struct {
	unsigned int counter;

	/* Parameters received by mock_on_sendv_fn() */
	struct proto *p;
	char data[262155];	/* accumulated iov[] data */
	int datalen;

	/* What mock_on_sendv_fn() should return when called */
	int retval;
} mock_on_sendv;

static struct {
	unsigned int counter;
	struct proto *p;
	char msg[16384];
} mock_on_error;

static const char *
msg_str(int id)
{
	static char other[10];
	switch (id) {
	case CMD_HELLO: return "CMD_HELLO";
	case CMD_SUB: return "CMD_SUB";
	case CMD_UNSUB: return "CMD_UNSUB";
	case CMD_GET: return "CMD_GET";
	case CMD_PUT: return "CMD_PUT";
	case CMD_BEGIN: return "CMD_BEGIN";
	case CMD_COMMIT: return "CMD_COMMIT";
	case CMD_PING: return "CMD_PING";
	case MSG_VERSION: return "MSG_VERSION";
	case MSG_INFO: return "MSG_INFO";
	case MSG_PONG: return "MSG_PONG";
	case MSG_ERROR: return "MSG_ERROR";
	case MSG_EOF: return "MSG_EOF";
	}
	snprintf(other, sizeof other, "%d", id);
	return other;
}

static void
fprinthex_cmp(FILE *f, const char *name, const char *data, unsigned int len,
			const char *data2, unsigned int len2)
{
	unsigned int i, j;


	fprintf(f, "%s [len %s%u%s] =\n", name,
		data2 && len != len2 ? cHILITE : "",
		len,
		data2 && len != len2 ? cNORMAL : "");
	for (i = 0; i < len; i += 16) {
		fprintf(f, "    ");
		for (j = i; j < i + 16; j++) {
		    if (j == i + 8) putc(' ', f);
		    if (j < len) {
			    int different = (data2 &&
				j < len2 && data2[j] != data[j]);
			    fprintf(f, "%s%02x%s ",
				different ? cHILITE : "",
				data[j] & 0xff,
				different ? cNORMAL : "");
		    } else
			    fprintf(f, "   ");
		}
		fprintf(f, "    ");
		for (j = i; j < i + 16 && j < len; j++) {
		    char ch = data[j];
		    int different = (data2 && j < len2 && data2[j] != data[j]);
		    if (ch < ' ' || ch > '~') ch = '.';
		    if (j == i + 8) putc(' ', f);
		    fprintf(f, "%s%c%s",
			    different ? cHILITE : "",
			    ch,
			    different ? cNORMAL : "");
		}
		putc('\n', f);
	}
}

static void
fprinthex(FILE *f, const char *name, const char *data, unsigned int len)
{
	fprinthex_cmp(f, name, data, len, NULL, 0);
}

/*
 * mock for the on_input() callback
 */

static int
mock_on_input_fn(struct proto *p, unsigned char msg,
	     const char *data, unsigned int datalen)
{
	mock_on_input.counter++;
	mock_on_input.p = p;
	mock_on_input.msg = msg;
	mock_on_input.datalen = datalen;
	memcpy(mock_on_input.data, data, datalen);

#if DEBUG
	dprintf("mock_on_input_fn(p=%p, msg=%s, datalen=%u) #%u\n",
		p, msg_str(msg), datalen, mock_on_input.counter);
	if (datalen)
		fprinthex(stderr, "data", data, datalen);
#endif
	return mock_on_input.retval;
}

static void
assert_mock_on_input(int line, const char *call, struct proto *p,
	unsigned char msg, const char *s, int slen)
{
	if (mock_on_input.counter != 1) {
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"fn was called %u time(s), expected 1\n",
			__FILE__, line, call, mock_on_input.counter);
		if (mock_on_sendv.counter) {
			/* Sometimes the error path ends up here,
			 * which is handy to see for diag. */
			fprintf(stderr, "and mock_on_sendv_fn()"
				" was called %u times:",
				mock_on_sendv.counter);
			fprinthex(stderr, "", mock_on_sendv.data,
				mock_on_sendv.datalen);
		}
		abort();
	}
	if (mock_on_input.p != p) {
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"expected proto %p, actual %p\n",
			__FILE__, line, call,
			p, mock_on_input.p);
		abort();
	}
	if (mock_on_input.msg != msg) {
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"expected msg %s, actual %s\n",
			__FILE__, line, call,
			msg_str(msg), msg_str(mock_on_input.msg));
		abort();
	}
	if (mock_on_input.datalen != slen ||
	    memcmp(mock_on_input.data, s, slen) != 0)
	{
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"data mismatch\n",
			__FILE__, line, call);
		fprinthex_cmp(stderr, "expected data", s, slen,
			mock_on_input.data, mock_on_input.datalen);
		fprinthex_cmp(stderr, "actual data", mock_on_input.data,
			mock_on_input.datalen, s, slen);
		abort();
	}
	/* Clear the mock_on_input structure after asserting */
	memset(&mock_on_input, 0, sizeof mock_on_input);
}
#define assert_mock_on_input(p, msg, s) \
	assert_mock_on_input(__LINE__, \
		"assert_mock_on_input(" #p ", " #msg ", " #s ")", \
		p, msg, s, sizeof s - 1)
#define assert_no_mock_on_input() \
	assert(mock_on_input.counter == 0)

/*
 * mock for the on_sendv() callback
 */

static int
mock_on_sendv_fn(struct proto *p, const struct iovec *iov, int niov)
{
	int i;

	mock_on_sendv.counter++;

	dprintf("mock_on_sendv_fn(p=%p, iov=%p, niov=%d) #%u\n", p, iov, niov,
		mock_on_sendv.counter);

	if (mock_on_sendv.p)
		assert(mock_on_sendv.p == p);
	mock_on_sendv.p = p;

	for (i = 0; i < niov; i++) {
		const struct iovec *io = &iov[i];
#if DEBUG
		if (i == 0 && io->iov_len)
			dprintf("  first byte: %s\n",
				msg_str(*(unsigned char *)io->iov_base));
		fprinthex(stderr, "  iov", io->iov_base, io->iov_len);
#endif
		if (!io->iov_len)
			continue;
		/* our mocked data[] buffer is only so big... */
		assert(mock_on_sendv.datalen + io->iov_len <
			sizeof mock_on_sendv.data);
		memcpy(&mock_on_sendv.data[mock_on_sendv.datalen],
			io->iov_base, io->iov_len);
		mock_on_sendv.datalen += io->iov_len;
	}

	return mock_on_sendv.retval;
}

static void
assert_mock_on_sendv(int line, const char *call, struct proto *p,
	const char *s, unsigned int slen)
{
	/* In case there was an error logged, show it now */
	if (mock_on_error.counter)
		fprintf(stderr, "%s:%d: last error: %s\n", __FILE__, line,
			mock_on_error.msg);

	if (slen && !mock_on_sendv.counter) {
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"mock_on_sendv_fn() was called 0 times\n",
			__FILE__, line, call);
		abort();
	}
	if (mock_on_sendv.p != p) {
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"expected proto %p, actual %p\n",
			__FILE__, line, call,
			p, mock_on_sendv.p);
		abort();
	}
	if (mock_on_sendv.datalen != slen ||
	    memcmp(mock_on_sendv.data, s, slen) != 0)
	{
		fprintf(stderr, "%s:%d: %s " FAILED ":\n\t"
			"data mismatch\n",
			__FILE__, line, call);
		fprinthex_cmp(stderr, "expected data", s, slen,
			mock_on_sendv.data, mock_on_sendv.datalen);
		fprinthex_cmp(stderr, "actual data", mock_on_sendv.data,
			mock_on_sendv.datalen, s, slen);
		abort();
	}
	/* Clear the mock_on_sendv structure after asserting */
	memset(&mock_on_sendv, 0, sizeof mock_on_sendv);

}
#define assert_mock_on_sendv(p, data) \
	assert_mock_on_sendv(__LINE__, \
		"assert_mock_on_sendv(" #p ", " #data ")", \
		p, data, sizeof data - 1)
#define assert_no_mock_on_sendv() \
	assert(mock_on_sendv.counter == 0)


static struct {
	unsigned int counter;
	void *udata;
} mock_udata_free;

static void
mock_udata_free_fn(void *udata)
{
	mock_udata_free.counter++;
	mock_udata_free.udata = udata;
}

static void
assert_mock_udata_free(int line, const char *call, void *udata)
{
	if (mock_udata_free.counter != 1) {
		fprintf(stderr, "%s:%d: %s " FAILED ": "
			"fn was called %u time(s), expected 1\n",
			__FILE__, line, call, mock_udata_free.counter);
		abort();
	}
	if (mock_udata_free.udata != udata) {
		fprintf(stderr, "%s:%d: %s " FAILED ": "
			"expected udata %p, actual %p\n",
			__FILE__, line, call, udata, mock_udata_free.udata);
		abort();
	}
	memset(&mock_udata_free, 0, sizeof mock_udata_free);
}
#define assert_mock_udata_free(udata) \
	assert_mock_udata_free(__LINE__, \
		"assert_mock_udata_free(" #udata ")", \
		udata)
#define assert_no_mock_udata_free() \
	assert(mock_udata_free.counter == 0)

static void
mock_on_error_fn(struct proto *p, const char *msg)
{
	dprintf("on_error: p=%p msg=<%s>\n", p, msg);
	mock_on_error.counter++;
	mock_on_error.p = p;
	snprintf(mock_on_error.msg, sizeof mock_on_error.msg, "%s", msg);
}

static void
mock_on_error_clear()
{
	mock_on_error.counter = 0;
	mock_on_error.p = NULL;
	mock_on_error.msg[0] = '\0';
}

static void
assert_mock_on_error_called(int line, const char *call, struct proto *p)
{
	if (mock_on_error.counter != 1) {
		fprintf(stderr, "%s:%d: %s " FAILED ": "
			"fn was called %u time(s), expected 1\n",
			__FILE__, line, call, mock_on_error.counter);
		abort();
	}
	if (mock_on_error.p != p) {
		fprintf(stderr, "%s:%d: %s " FAILED ": "
			"on_error was called with p=%p, expected %p\n",
			__FILE__, line, call, mock_on_error.p, p);
		abort();
	}
	if (!mock_on_error.msg[0]) {
		fprintf(stderr, "%s:%d: %s " FAILED ": "
			"on_error was called with msg=\"\"\n",
			__FILE__, line, call);
		abort();
	}
	mock_on_error_clear();
}

#define assert_mock_on_error_called(p) \
	assert_mock_on_error_called(__LINE__, \
		"assert_mock_on_error_called(" #p ")", p)

/* -- binary protocol tests -- */

static void
test_binary_proto()
{
	struct proto *p;

	p = proto_new();

	/* Prepare to capture callbacks */
	mock_on_error_clear();
	proto_set_on_error(p, mock_on_error_fn);
	proto_set_on_input(p, mock_on_input_fn);

	/* Initially the protocol mode is unknown */
	assert(proto_get_mode(p) == PROTO_MODE_UNKNOWN);

	/* Auto-detect on the first hello message */
	assert(proto_recv(p, "\x00\0\6\3Hello", 9) == 9);
	assert(proto_get_mode(p) == PROTO_MODE_BINARY);
	assert_mock_on_input(p, CMD_HELLO, "\3Hello");

	/* Exercise receiving all of the message types [from net] */
	assert(proto_recv(p, "\x00\0\1\3", 4) == 4);
	assert_mock_on_input(p, CMD_HELLO, "\3");
	assert(proto_recv(p, "\x01\0\4test", 7) == 7);
	assert_mock_on_input(p, CMD_SUB, "test");
	assert(proto_recv(p, "\x02\0\4yeah", 7) == 7);
	assert_mock_on_input(p, CMD_UNSUB, "yeah");
	assert(proto_recv(p, "\x03\0\3key", 6) == 6);
	assert_mock_on_input(p, CMD_GET, "key");
	assert(proto_recv(p, "\x04\0\3key", 6) == 6);
	assert_mock_on_input(p, CMD_PUT, "key");
	assert(proto_recv(p, "\x04\0\013key\0val\0nul", 14) == 14);
	assert_mock_on_input(p, CMD_PUT, "key\0val\0nul");
	assert(proto_recv(p, "\x05\0\0", 3) == 3);
	assert_mock_on_input(p, CMD_BEGIN, "");
	assert(proto_recv(p, "\x06\0\0", 3) == 3);
	assert_mock_on_input(p, CMD_COMMIT, "");
	assert(proto_recv(p, "\x07\0\0", 3) == 3);
	assert_mock_on_input(p, CMD_PING, "");
	assert(proto_recv(p, "\x07\0\1x", 4) == 4);
	assert_mock_on_input(p, CMD_PING, "x");

	assert(proto_recv(p, "\x80\0\4\3foo", 7) == 7);
	assert_mock_on_input(p, MSG_VERSION, "\3foo");
	assert(proto_recv(p, "\x81\0\3key", 6) == 6);
	assert_mock_on_input(p, MSG_INFO, "key");
	assert(proto_recv(p, "\x81\0\013key\0val\0nul", 14) == 14);
	assert_mock_on_input(p, MSG_INFO, "key\0val\0nul");
	assert(proto_recv(p, "\x82\0\0", 3) == 3);
	assert_mock_on_input(p, MSG_PONG, "");
	assert(proto_recv(p, "\x82\0\1x", 4) == 4);
	assert_mock_on_input(p, MSG_PONG, "x");
	assert(proto_recv(p, "\x83\0\5error", 8) == 8);
	assert_mock_on_input(p, MSG_ERROR, "error");

	/* Exercise sending all of the message types [to net] */
	proto_set_on_sendv(p, mock_on_sendv_fn);
	assert(proto_output(p, "%c %c %s", CMD_HELLO, 0, "hello") != -1);
	assert_mock_on_sendv(p, "\x00\0\6\0hello");
	assert(proto_output(p, "%c %c", CMD_HELLO, 0) != -1);
	assert_mock_on_sendv(p, "\x00\0\1\0");
	assert(proto_output(p, "%c %s", CMD_SUB, "*") != -1);
	assert_mock_on_sendv(p, "\x01\0\1*");
	assert(proto_output(p, "%c %s", CMD_UNSUB, "*") != -1);
	assert_mock_on_sendv(p, "\x02\0\1*");
	assert(proto_output(p, "%c %s", CMD_GET, "key") != -1);
	assert_mock_on_sendv(p, "\x03\0\3key");

	/* exercising CMD_PUT in its various ways [to net] */
	assert(proto_output(p, "%c %s", CMD_PUT, "key") != -1);
	assert_mock_on_sendv(p, "\x04\0\3key");
	assert(proto_output(p, "%c %s%c%s", CMD_PUT, "key", 0, "val") != -1);
	assert_mock_on_sendv(p, "\x04\0\7key\0val");
	assert(proto_output(p, "%c %*s%c%s", CMD_PUT, 3, "key", 0, "val") !=-1);
	assert_mock_on_sendv(p, "\x04\0\7key\0val");
	assert(proto_output(p, "%c %*s", CMD_PUT, 7, "key\0val") != -1);
	assert_mock_on_sendv(p, "\x04\0\7key\0val");
	assert(proto_output(p, "%c", CMD_BEGIN) != -1);
	assert_mock_on_sendv(p, "\x05\0\0");
	assert(proto_output(p, "%c", CMD_COMMIT) != -1);
	assert_mock_on_sendv(p, "\x06\0\0");
	assert(proto_output(p, "%c", CMD_PING) != -1);
	assert_mock_on_sendv(p, "\x07\0\0");
	assert(proto_output(p, "%c %s", CMD_PING, "abcd") != -1);
	assert_mock_on_sendv(p, "\x07\0\4abcd");

	assert(proto_output(p, "%c %c", MSG_VERSION, 9) != -1);
	assert_mock_on_sendv(p, "\x80\0\1\x9");
	assert(proto_output(p, "%c %c %s", MSG_VERSION, 9, "") != -1);
	assert_mock_on_sendv(p, "\x80\0\1\x9");
	assert(proto_output(p, "%c %c %s", MSG_VERSION, 9, "wxyz") != -1);
	assert_mock_on_sendv(p, "\x80\0\5\x9wxyz");
	assert(proto_output(p, "%c %s", MSG_INFO, "key") != -1);
	assert_mock_on_sendv(p, "\x81\0\3key");
	assert(proto_output(p, "%c %*s", MSG_INFO, 10, "key\0val\0ue") != -1);
	assert_mock_on_sendv(p, "\x81\0\012key\0val\0ue");
	assert(proto_output(p, "%c %s%c%*s",MSG_INFO,"key",0,6,"val\0ue")!=-1);
	assert_mock_on_sendv(p, "\x81\0\012key\0val\0ue");
	assert(proto_output(p, "%c", MSG_PONG) != -1);
	assert_mock_on_sendv(p, "\x82\0\0");
	assert(proto_output(p, "%c %s", MSG_PONG, "abcd") != -1);
	assert_mock_on_sendv(p, "\x82\0\4abcd");
	assert(proto_output(p, "%c %s", MSG_ERROR, "abcd") != -1);
	assert_mock_on_sendv(p, "\x83\0\4abcd");

	/* Exercise some malformed formats [to net] */
	assert(proto_output(p, " ") == -1);
	assert_no_mock_on_sendv();
	assert_mock_on_error_called(p);
	assert(proto_output(p, "%%") == -1);
	assert_no_mock_on_sendv();
	assert_mock_on_error_called(p);
	assert(proto_output(p, "%x", 1) == -1);
	assert_no_mock_on_sendv();
	assert_mock_on_error_called(p);
	assert(proto_output(p, "x") == -1);
	assert_no_mock_on_sendv();
	assert_mock_on_error_called(p);
	assert(proto_output(p, "%*s", 0x20000, "") == -1);
	assert_no_mock_on_sendv();
	assert_mock_on_error_called(p);
	assert(proto_output(p, "%c %*s", 1, 0x20000, "") == -1);
	assert(errno == ENOMEM);
	assert_no_mock_on_sendv();
	mock_on_error_clear();

	proto_free(p);
}

/* -- text protocol tests -- */

static void
test_text_proto()
{
	struct proto *p;

	p = proto_new();

	/* Initially the protocol mode is unknown */
	assert(proto_get_mode(p) == PROTO_MODE_UNKNOWN);

	/* Prepare capturing on_input() callbacks */
	proto_set_on_input(p, mock_on_input_fn);

#define assert_proto_recv(p, s) \
	assert(proto_recv(p, s, sizeof s - 1) == sizeof s - 1)

	/* Recieving a text message auto-detects text mode */
	assert_proto_recv(p, "hello 0\n");
	assert(proto_get_mode(p) == PROTO_MODE_TEXT);
	assert_mock_on_input(p, CMD_HELLO, "\0");

	/* Exercise receiving various quoting styles [from net] */
	assert_proto_recv(p, "hello 1 woo\n");
	assert_mock_on_input(p, CMD_HELLO, "\1woo");
	assert_proto_recv(p, "hello 2 \"woo\"\n");
	assert_mock_on_input(p, CMD_HELLO, "\2woo");
	assert_proto_recv(p, "hello 2 \"w oo\"\n");
	assert_mock_on_input(p, CMD_HELLO, "\2w oo");
	assert_proto_recv(p, "hello 3 w oo\n");
	assert_mock_on_input(p, CMD_HELLO, "\3w oo");
	assert_proto_recv(p, "hello 3 w oo \n");
	assert_mock_on_input(p, CMD_HELLO, "\3w oo");
	assert_proto_recv(p, " hello  3  w  oo  \n");
	assert_mock_on_input(p, CMD_HELLO, "\3w  oo");
	assert_proto_recv(p, "hello 3 w\"oo \n");
	assert_mock_on_input(p, CMD_HELLO, "\3w\"oo");
	assert_proto_recv(p, "hello 3 \"w\\042oo\"\n");
	assert_mock_on_input(p, CMD_HELLO, "\3w\"oo");
	assert_proto_recv(p, "hello 3 \"w\\040oo\"\n");
	assert_mock_on_input(p, CMD_HELLO, "\3w oo");
	assert_proto_recv(p, "hello 3 \"w oo\"\n");
	assert_mock_on_input(p, CMD_HELLO, "\3w oo");

	/* Exercise receiving blank lines [from net] */
	assert_proto_recv(p, "\n");
	assert_no_mock_on_input();
	assert_proto_recv(p, "\r\n\r\n");
	assert_no_mock_on_input();

	/* Exercise encoding in text [to net].
	 * (This relies on the proto receiving one text message
	 * earlier, otherwise it would be in binary mode) */
	proto_set_on_sendv(p, mock_on_sendv_fn);
	assert(proto_output(p, "%c %c %s", CMD_HELLO, 0, "hello") != -1);
	assert_mock_on_sendv(p, "HELLO 0 hello\r\n");

	/* Exercise recv other messages [from net] */
	assert_proto_recv(p, "sub *\n");
	assert_mock_on_input(p, CMD_SUB, "*");
	assert_proto_recv(p, "unSUB *\n");
	assert_mock_on_input(p, CMD_UNSUB, "*");
	assert_proto_recv(p, "GET key\n");
	assert_mock_on_input(p, CMD_GET, "key");
	assert_proto_recv(p, "put key\n");
	assert_mock_on_input(p, CMD_PUT, "key");
	assert_proto_recv(p, "put key value\n");
	assert_mock_on_input(p, CMD_PUT, "key\0value");
	assert_proto_recv(p, "put key \"\"\n");
	assert_mock_on_input(p, CMD_PUT, "key\0");
	assert_proto_recv(p, "put \"key\" \"\"\n");
	assert_mock_on_input(p, CMD_PUT, "key\0");
	assert_proto_recv(p, "begin\r\n");
	assert_mock_on_input(p, CMD_BEGIN, "");
	assert_proto_recv(p, "commit\r\n");
	assert_mock_on_input(p, CMD_COMMIT, "");
	assert_proto_recv(p, "ping\r\n");
	assert_mock_on_input(p, CMD_PING, "");
	assert_proto_recv(p, "ping 12345\r\n");
	assert_mock_on_input(p, CMD_PING, "12345");
	assert_proto_recv(p, "version 255\r\n");
	assert_mock_on_input(p, MSG_VERSION, "\xff");
	assert_proto_recv(p, "version 255 yay for me\r\n");
	assert_mock_on_input(p, MSG_VERSION, "\xffyay for me");
	assert_proto_recv(p, "info key\r\n");
	assert_mock_on_input(p, MSG_INFO, "key");
	assert_proto_recv(p, "info key value\r\n");
	assert_mock_on_input(p, MSG_INFO, "key\0value");
	assert_proto_recv(p, "info key \"\"\r\n");
	assert_mock_on_input(p, MSG_INFO, "key\0");
	assert_proto_recv(p, "info key value space\r\n");
	assert_mock_on_input(p, MSG_INFO, "key\0value space");
	assert_proto_recv(p, "info key \"value\\000space\"\r\n");
	assert_mock_on_input(p, MSG_INFO, "key\0value\0space");
	assert_proto_recv(p, "info key value\0space\r\n");
	assert_mock_on_input(p, MSG_INFO, "key\0value\0space");
	assert_proto_recv(p, "pong\n");
	assert_mock_on_input(p, MSG_PONG, "");
	assert_proto_recv(p, "pong  xyz \n");
	assert_mock_on_input(p, MSG_PONG, "xyz");
	assert_proto_recv(p, "error text\n");
	assert_mock_on_input(p, MSG_ERROR, "text");
	assert_proto_recv(p, "error \"\"\n");
	assert_mock_on_input(p, MSG_ERROR, "");

	/* Exercise recveing a bad command [from net] */
	assert_proto_recv(p, "commit foo\r\n");
	assert_no_mock_on_input();
	assert_mock_on_sendv(p, "ERROR \"unexpected arg for 'commit'\"\r\n");


	/* Exercise sensing messages to the net */
	assert(proto_output(p, "%c %c %s", CMD_HELLO, 0, "hello") != -1);
	assert_mock_on_sendv(p, "HELLO 0 hello\r\n");
	assert(proto_output(p, "%c %c", CMD_HELLO, 0) != -1);
	assert_mock_on_sendv(p, "HELLO 0\r\n");
	assert(proto_output(p, "%c %s", CMD_SUB, "*") != -1);
	assert_mock_on_sendv(p, "SUB *\r\n");
	assert(proto_output(p, "%c %s", CMD_UNSUB, "*") != -1);
	assert_mock_on_sendv(p, "UNSUB *\r\n");
	assert(proto_output(p, "%c %s", CMD_GET, "key") != -1);
	assert_mock_on_sendv(p, "GET key\r\n");

	/* exercising CMD_PUT in its various ways [to net] */
	assert(proto_output(p, "%c %s", CMD_PUT, "key") != -1);
	assert_mock_on_sendv(p, "PUT key\r\n");
	assert(proto_output(p, "%c %s%c%s", CMD_PUT, "key", 0, "val") != -1);
	assert_mock_on_sendv(p, "PUT key val\r\n");
	assert(proto_output(p, "%c %*s%c%s", CMD_PUT, 3, "key", 0, "val") !=-1);
	assert_mock_on_sendv(p, "PUT key val\r\n");
	assert(proto_output(p, "%c %*s", CMD_PUT, 7, "key\0val") != -1);
	assert_mock_on_sendv(p, "PUT key val\r\n");
	assert(proto_output(p, "%c", CMD_BEGIN) != -1);
	assert_mock_on_sendv(p, "BEGIN\r\n");
	assert(proto_output(p, "%c", CMD_COMMIT) != -1);
	assert_mock_on_sendv(p, "COMMIT\r\n");
	assert(proto_output(p, "%c", CMD_PING) != -1);
	assert_mock_on_sendv(p, "PING\r\n");
	assert(proto_output(p, "%c %s", CMD_PING, "abcd") != -1);
	assert_mock_on_sendv(p, "PING abcd\r\n");

	assert(proto_output(p, "%c %c", MSG_VERSION, 9) != -1);
	assert_mock_on_sendv(p, "VERSION 9\r\n");
	assert(proto_output(p, "%c %c %s", MSG_VERSION, 9, "") != -1);
	assert_mock_on_sendv(p, "VERSION 9 \"\"\r\n");
	assert(proto_output(p, "%c %c %s", MSG_VERSION, 9, "wxyz") != -1);
	assert_mock_on_sendv(p, "VERSION 9 wxyz\r\n");
	assert(proto_output(p, "%c %s", MSG_INFO, "key") != -1);
	assert_mock_on_sendv(p, "INFO key\r\n");
	assert(proto_output(p, "%c %*s", MSG_INFO, 10, "key\0val\nue") != -1);
	assert_mock_on_sendv(p, "INFO key \"val\\012ue\"\r\n");
	assert(proto_output(p, "%c %s%c%*s",MSG_INFO,"key",0,6,"val\nue")!=-1);
	assert_mock_on_sendv(p, "INFO key \"val\\012ue\"\r\n");
	assert(proto_output(p, "%c", MSG_PONG) != -1);
	assert_mock_on_sendv(p, "PONG\r\n");
	assert(proto_output(p, "%c %s", MSG_PONG, "abcd") != -1);
	assert_mock_on_sendv(p, "PONG abcd\r\n");
	assert(proto_output(p, "%c %s", MSG_ERROR, "abcd") != -1);
	assert_mock_on_sendv(p, "ERROR abcd\r\n");

	/* Exercise some malformed formats [to net] */
	assert(proto_output(p, " ") == -1);
	assert_no_mock_on_sendv();
	assert(proto_output(p, "%%") == -1);
	assert_no_mock_on_sendv();
	assert(proto_output(p, "%x", 1) == -1);
	assert_no_mock_on_sendv();
	assert(proto_output(p, "x") == -1);
	assert_no_mock_on_sendv();

	char mega[65536 + 1]; /* too big */
	memset(mega, 'M', sizeof mega);
	assert(proto_output(p, "%*s", (int)sizeof mega, mega) == -1);
	assert_no_mock_on_sendv();
	assert(proto_output(p, "%c %*s", 1, (int)sizeof mega, mega) == -1);
	assert_no_mock_on_sendv();

	proto_free(p);
}

int
main()
{
	struct proto *p;
	static char udata0[] = "udata0";
	static char udata1[] = "udata1";

	/* Can allocate and destroy */
	p = proto_new();
	assert(p);
	proto_free(p);

	/* Can set and change the udata, and it is freed correctly */
	p = proto_new();
		assert(!proto_get_udata(p));
	proto_set_udata(p, udata0, mock_udata_free_fn);	/* udata0, fn */
		assert_no_mock_udata_free();
		assert(proto_get_udata(p) == udata0);
	proto_set_udata(p, udata1, NULL);		/* udata1, NULL */
		assert_mock_udata_free(udata0);
		assert(proto_get_udata(p) == udata1);
	proto_set_udata(p, NULL, mock_udata_free_fn);	/* NULL, fn */
		assert_no_mock_udata_free();
		assert(!proto_get_udata(p));
	proto_set_udata(p, udata1, mock_udata_free_fn); /* udata1, fn */
		assert_mock_udata_free(NULL);
		assert(proto_get_udata(p) == udata1);
	proto_free(p);
		assert_mock_udata_free(udata1);

	test_binary_proto();
	test_text_proto();

	dprintf("%s:%d: %s\n", __FILE__, __LINE__, PASSED);
}
