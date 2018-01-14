/*
 * Unit tests for the library API info.h
 * Mocks the client caller, and the proto layer.
 */

#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/uio.h>

#include "info.h"
#include "proto.h"
#include "sockunix.h"

/*------------------------------------------------------------
 * Mock socket
 * Instead of connecting to a real server, we use a self-pipe
 * which the library will read() on and pass to our mocked
 * proto_recv(). We also mock the proto_outputv() function for
 * messages that the library will send.
 */

static struct {
	int fd[2];
} mock_socket;

static void
mock_socket_reset()
{

	if (mock_socket.fd[0] != -1) {
		(void) close(mock_socket.fd[0]);
		mock_socket.fd[0] = -1;
	}
	if (mock_socket.fd[1] != -1) {
		(void) close(mock_socket.fd[1]);
		mock_socket.fd[1] = -1;
	}
}

static void
mock_socket_init()
{
	mock_socket.fd[0] = -1;
	mock_socket.fd[1] = -1;
	atexit(mock_socket_reset);
}

/* Test if the library called close() on the socket returned
 * from sockunix_connect(). */
static int
mock_socket_was_closed()
{
	int ret = fcntl(mock_socket.fd[0], F_GETFL);
	return ret == -1 && errno == EBADF;
}
static int
mock_socket_is_open()
{
	return !mock_socket_was_closed();
}

/* Schedule byte c to appear next when the library calls
 * read() on the socket.
 * If c is EOF, then schedule an EOF (0-length read) to
 * appear on next read().
 * (We expect the library to immediately pass the read
 * byte to our mocked proto_recv())
 */
static void
mock_socket_next_read_returns(int c)
{
	assert(!mock_socket_was_closed());
	if (c == EOF) {
		assert(close(mock_socket.fd[1]) != -1);
		mock_socket.fd[1] = -1;
	} else {
		char ch = c;
		assert(write(mock_socket.fd[1], &ch, 1) == 1);
	}
}

int
sockunix_connect()
{
	assert(mock_socket_was_closed());
	mock_socket_reset();
	assert(pipe(mock_socket.fd) == 0);
	return mock_socket.fd[0];
}

#ifndef SMALL
#include <netdb.h>
int
tcp_client_addrinfo(const char *hostport, struct addrinfo **res)
{
	*res = NULL;
	errno = ENOTSUP;
	return EAI_SYSTEM;
}
#endif

/*------------------------------------------------------------
 * Mock proto
 */

static struct proto {
	int mode;
        int (*on_sendv)(struct proto *p, const struct iovec *iovs, int niovs);
	int (*on_input)(struct proto *p, unsigned char msg,
	                               const char *data, unsigned int datalen);

} mock_proto;

void
proto_free(struct proto *p)
{
}

struct proto *
proto_new() {
	memset(&mock_proto, 0, sizeof mock_proto);
	return &mock_proto;
}

int
proto_set_mode(struct proto *p, int mode)
{
	assert(p == &mock_proto);
	p->mode = mode;
	return 0;
}

void proto_set_on_sendv(struct proto *p,
        int (*on_sendv)(struct proto *p, const struct iovec *iovs, int niovs))
{
	assert(p == &mock_proto);
	p->on_sendv = on_sendv;
}

void proto_set_on_input(struct proto *p,
	int (*on_input)(struct proto *p, unsigned char msg,
		        const char *data, unsigned int datalen))
{
	assert(p == &mock_proto);
	p->on_input = on_input;
}


/* mock/expect proto_outputv() call */

struct loc {
	const char *file;	/* __FILE__ */
	int lineno;		/* __LINE__ */
};

static struct proto_output_call {
	unsigned char msg;
	unsigned int datalen;
#define DATA_IGNORE 9999
	char data[128];
	int retval;
	struct loc loc;
} proto_output_calls[32];
unsigned int expected_proto_output_calls;
unsigned int actual_proto_output_calls;

/* Helper function to convert parameters into a call record structure */
static void
record_proto_outputv(struct proto_output_call *c, unsigned char msg,
	const char *fmt, va_list ap)
{
	char *src;
	unsigned int srclen;
	char f;
	char ch;

	c->msg = msg;
	if (strcmp(fmt, "*") == 0) {   /* fmt="*" means ignore data */
		c->datalen = DATA_IGNORE;
		return;
	}
	c->datalen = 0;
	while ((f = *fmt++)) {
		if (f == ' ') continue;
		assert(f == '%');
		switch (*fmt++) {
		case '*':
			f = *fmt++; assert(f == 's');
			srclen = va_arg(ap, int);
			src = va_arg(ap, char *);
			break;
		case 's':
			src = va_arg(ap, char *);
			srclen = strlen(src);
			break;
		case 'c':
			ch = va_arg(ap, int) & 0xff;
			src = &ch;
			srclen = 1;
			break;
		default:
			assert(!"invalid fmt");
		}
		assert(c->datalen + srclen <= sizeof c->data);
		memcpy(&c->data[c->datalen], src, srclen);
		c->datalen += srclen;
	}
}

/* Add an expected call to the expected list. Also record what
 * return value should be returned when that call occurs.
 * When our mock proto_outputv() is invoked by the UUT it
 * will check that the invocation matches this one. */
__attribute__((format(printf,5,6)))
static struct proto_output_call *
expect_proto_output_(const char *file, int lineno, int retval, unsigned char msg, const char *fmt, ...)
{
	struct proto_output_call *c =
		&proto_output_calls[expected_proto_output_calls++];
	va_list ap;

	va_start(ap, fmt);
	record_proto_outputv(c, msg, fmt, ap);
	va_end(ap);
	c->retval = retval;
	c->loc.file = file;
	c->loc.lineno = lineno;
	return c;
}
#define expect_proto_output(...) \
	expect_proto_output_(__FILE__, __LINE__, __VA_ARGS__)

static const char * msg_name[0x100] = {
	[CMD_HELLO] = "CMD_HELLO",
	[CMD_SUB] = "CMD_SUB",
	[CMD_UNSUB] = "CMD_UNSUB",
	[CMD_READ] = "CMD_READ",
	[CMD_WRITE] = "CMD_WRITE",
	[CMD_BEGIN] = "CMD_BEGIN",
	[CMD_COMMIT] = "CMD_COMMIT",
	[CMD_PING] = "CMD_PING",
	[MSG_VERSION] = "MSG_VERSION",
	[MSG_INFO] = "MSG_INFO",
	[MSG_PONG] = "MSG_PONG",
	[MSG_ERROR] = "MSG_ERROR",
	[MSG_EOF] = "MSG_EOF"
};

static void
print_expected_proto_output(FILE *f)
{
	unsigned int i;

	fprintf(f, "expected_proto_output[] = {\n");
	for (i = 0; i < expected_proto_output_calls; i++) {
		const struct proto_output_call *c = &proto_output_calls[i];
		fprintf(f, " #%u msg=%x(%s) ", i, c->msg, msg_name[c->msg]);
		if (c->datalen != DATA_IGNORE) {
			fprintf(f, "datalen=%u data=\"%.*s\"", c->datalen,
				c->datalen, c->data);
		}
		fprintf(f, " (%s:%d)\n", c->loc.file, c->loc.lineno);
	}
	fprintf(f, "}\n");
}

/* Mocked interface to the protocol translator. Simply verify that
 * the arguments are what are expected, and return the value recorded. */
int
proto_output(struct proto *p, unsigned char msg, const char *fmt, ...)
{
	struct proto_output_call actual;
	struct proto_output_call *expected;
	va_list ap;

	memset(&actual, 0, sizeof actual);
	va_start(ap, fmt);
	record_proto_outputv(&actual, msg, fmt, ap);
	va_end(ap);

	assert(actual_proto_output_calls < expected_proto_output_calls);
	expected = &proto_output_calls[actual_proto_output_calls++];
	assert(p == &mock_proto);
	if (actual.msg != expected->msg) {
		print_expected_proto_output(stderr);
		fprintf(stderr, "%s:%d: #%u "
			"expected msg=%x(%s) but got msg=%x(%s) data=\"%.*s\"\n",
			expected->loc.file, expected->loc.lineno,
			actual_proto_output_calls - 1,
			expected->msg, msg_name[expected->msg],
			actual.msg, msg_name[actual.msg],
			actual.datalen, actual.data);
	}
	assert(actual.msg == expected->msg);
	if (expected->datalen != DATA_IGNORE)
		assert(actual.datalen == expected->datalen &&
		       memcmp(actual.data, expected->data,
			      actual.datalen) == 0);
	return expected->retval;
}

/* Check that all expected proto_output() calls have occurred */
static int
proto_output_check()
{
	int ok = actual_proto_output_calls == expected_proto_output_calls;
	actual_proto_output_calls = 0;
	expected_proto_output_calls = 0;
	return ok;
}

/* mock/expect proto_recv()/on_input() call */

static struct on_input_call {
	unsigned char msg;
	char data[32];
	unsigned int datalen;
	int retval;
	struct loc loc;
} on_input_call[16];
static unsigned int expected_on_input_calls;
static unsigned int actual_on_input_calls;

/* Future expectation that on_input() will return retval after being called
 * with the given arguments via proto_recv() */
static struct on_input_call *
expect_on_input_(const char *file, int lineno, int retval, unsigned char msg, const char *data,
		unsigned int datalen)
{
	struct on_input_call *c = &on_input_call[expected_on_input_calls++];
	c->msg = msg;
	c->loc.file = file;
	c->loc.lineno = lineno;
	assert(datalen < sizeof c->data);
	c->datalen = datalen;
	memcpy(c->data, data, datalen);
	c->retval = retval; /* expected retval */
	mock_socket_next_read_returns('r');
	return c;
}
#define expect_on_input(retval, msg, data) \
	expect_on_input_(__FILE__, __LINE__, retval, msg, data, sizeof data - 1)

int
proto_recv(struct proto *p, const void *net, unsigned int netlen)
{
	int retval = -1;
	struct on_input_call *expected;

	assert(p == &mock_proto);
	assert(mock_proto.on_input);

	if (netlen == 0)
		return 0; /* XXX todo */

	/* Multiple writes of one char to the write end of the pipe
	 * will be seen here as one read of multiple bytes.
	 * Split that up. */
	assert(netlen > 0);
	while (netlen--) {
		/* Make each write to mock_socket_next_read_returns('r')
		 * trigger a call on_input() */
		assert(actual_on_input_calls < expected_on_input_calls);
		expected = &on_input_call[actual_on_input_calls++];
		retval = mock_proto.on_input(p, expected->msg, expected->data,
			expected->datalen);
		if (retval != expected->retval)
		    fprintf(stderr, "%s:%d: proto_recv:"
			"#%u on_input() returned %d but expected %d\n",
			expected->loc.file, expected->loc.lineno,
			actual_on_input_calls - 1,
			retval, expected->retval);
		assert(retval == expected->retval);
	}
	return retval;
}

static int
proto_recv_check()
{
	int ok = actual_on_input_calls == expected_on_input_calls;
	actual_on_input_calls = 0;
	expected_on_input_calls = 0;
	return ok;
}

#define CHECK() do { \
	assert(proto_output_check()); \
	assert(proto_recv_check()); \
} while (0)

/*------------------------------------------------------------
 * tests
 */

int
main()
{
	mock_socket_init();

	/* You can close before any opens */
	{
	    assert(!mock_socket_is_open());
	    info_close();
	    info_close();
	    assert(!mock_socket_is_open());
	}

	assert(info_open(NULL) != -1);

	/* You can call info_read() to get a single value */
	{
	    char buf[8] = "";
	    expect_proto_output(1, CMD_READ, "%s", "key");
	    expect_on_input(1, MSG_INFO, "key\0value");
	    assert(info_read("key", buf, sizeof buf) > 0);
	    assert(strncmp(buf, "value", 5) == 0);
	    CHECK();
	}

	/* You call info_read() to get a deleted value,
	 * because it returns 0 */
	{
	    char buf[8] = "x";
	    expect_proto_output(1, CMD_READ, "%s", "key");
	    expect_on_input(1, MSG_INFO, "key");
	    assert(info_read("key", buf, sizeof buf) == 0);
	    assert(buf[0] == 'x'); /* unchanged buf */
	    CHECK();
	}

	/* You can call info_write() to write a single value */
	{
	    expect_proto_output(1, CMD_WRITE, "%s%c%s", "key", 0, "value");
	    assert(info_write("key", "value", 5) != -1);
	    CHECK();
	}

	/* You can call info_write() to delete a single value */
	{
	    expect_proto_output(1, CMD_WRITE, "%s", "key");
	    assert(info_write("key", NULL, 0) != -1);
	    CHECK();
	}

	/* You can call info_delete() to delete a single value */
	{
	    expect_proto_output(1, CMD_WRITE, "%s", "key");
	    assert(info_delete("key") != -1);
	    CHECK();
	}

	/* You can call info_exists() to test a value exists or not */
	{
	    expect_proto_output(1, CMD_READ, "%s", "key");
	    expect_on_input(1, MSG_INFO, "key\0"); /* empty value */
	    assert(info_exists("key") == 1);
	    CHECK();

	    expect_proto_output(1, CMD_READ, "%s", "key");
	    expect_on_input(1, MSG_INFO, "key\0xyz");
	    assert(info_exists("key") == 1);
	    CHECK();

	    expect_proto_output(1, CMD_READ, "%s", "key");
	    expect_on_input(1, MSG_INFO, "key");
	    assert(info_exists("key") == 0);
	    CHECK();
	}

#define BAD_BUF (char *)1, ~0

	/* You can call info_readv() to get two coherent values,
	 * even if one is deleted */
	{
	    struct info_bind binds[3] = {
		  { "key1", BAD_BUF },
		  { "key2", BAD_BUF },
		  { NULL } };
	    char buf[24];

	    expect_proto_output(1, CMD_BEGIN, "");
	    expect_proto_output(1, CMD_READ, "%s", "key1");
	    expect_proto_output(1, CMD_READ, "%s", "key2");
	    expect_proto_output(1, CMD_PING, "");
	    expect_proto_output(1, CMD_COMMIT, "");

	    expect_on_input(1, MSG_INFO, "key1\0val");
	    expect_on_input(1, MSG_INFO, "key2");
	    expect_on_input(1, MSG_PONG, "");

	    assert(info_readv(binds, buf, sizeof buf) != -1);

	    CHECK();
	    assert(binds[0].valuesz == 3);
	    assert(strncmp(binds[0].value, "val", 3) == 0);
	    assert(binds[1].value == NULL);
	    assert(binds[1].valuesz == 0);
	}

	/* You can call info_writev() to write two coherent values,
	 * on of which is deleted. */
	{
	    struct info_bind binds[3] = {
		  { "key1", "value", 5 },
		  { "key2", NULL, 0 },
		  { NULL } };

	    expect_proto_output(1, CMD_BEGIN, "");
	    expect_proto_output(1, CMD_WRITE, "%s%c%*s", "key1", 0, 5, "value");
	    expect_proto_output(1, CMD_WRITE, "%s", "key2");
	    expect_proto_output(1, CMD_COMMIT, "");

	    assert(info_writev(binds) != -1);

	    CHECK();
	}

	assert(info_fileno() != -1);
	assert(info_fileno() == mock_socket.fd[0]);

	/* We can do an empty transaction */
	{
	    expect_proto_output(1, CMD_BEGIN, "");
	    expect_proto_output(1, CMD_PING, "*");
	    expect_proto_output(1, CMD_COMMIT, "");
	    expect_on_input(1, MSG_PONG, "");

	    assert(info_tx_begin() != -1);
	    assert(info_tx_commit(NULL) != -1);
	    CHECK();
	}

	/* We _can't_ do transactions outside of a tx_begin */
	{
	    assert(info_tx_read("key1") == -1);
	    assert(info_tx_write("key2", "value", 5) == -1);
	    assert(info_tx_delete("key3") == -1);
	    assert(info_tx_sub("pattern") == -1);
	}

	/* We can do transaction with many things */
	{
	    expect_proto_output(1, CMD_BEGIN, "");
	    expect_proto_output(1, CMD_READ, "%s", "key1");
	    expect_proto_output(1, CMD_WRITE, "%s%c%s", "key2", 0, "value");
	    expect_proto_output(1, CMD_WRITE, "%s", "key3");
	    expect_proto_output(1, CMD_SUB, "%s", "pattern");
	    expect_proto_output(1, CMD_UNSUB, "%s", "pattern");
	    expect_proto_output(1, CMD_PING, "*");
	    expect_proto_output(1, CMD_COMMIT, "");

	    expect_on_input(1, MSG_PONG, "");

	    assert(info_tx_begin() != -1);
	    assert(info_tx_read("key1") != -1);
	    assert(info_tx_write("key2", "value", 5) != -1);
	    assert(info_tx_delete("key3") != -1);
	    assert(info_tx_sub("pattern") != -1);
	    assert(info_tx_unsub("pattern") != -1);
	    assert(info_tx_commit(NULL) != -1);

	    CHECK();
	}

	/* More tests needed */

}
