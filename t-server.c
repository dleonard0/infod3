#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

#include <sys/socket.h>
#include <sys/un.h>

#define DEBUG 0

/* Check for -1 return and raise an error.
 * Returns e */
static int
check(const char *file, int line, const char *estr, int e)
{
	if (e < 0) {
		fprintf(stderr, "%s:%d: check(%s) failed\n\t%d %s\n",
			file, line, estr, e, strerror(errno));
		abort();
	}
	return e;
}
#define CHECK(e) check(__FILE__, __LINE__, #e, e)

/* -- local test AF_UNIX socket management -- */

static char sock_path[256];

static void
cleanup()
{
	unlink(sock_path);
}

static void
make_sun(struct sockaddr_un *sun, const char *path)
{
	memset(sun, 0, sizeof *sun);
	sun->sun_family = AF_UNIX;
	snprintf(sun->sun_path, sizeof sun->sun_path, "%s", path);
}

/* Create a new listening socket fd, listening on sock_path[] */
static int
listen_local()
{
	int fd;
	struct sockaddr_un sun;

	assert(!sock_path[0]); /* only called once */
	snprintf(sock_path, sizeof sock_path, "/tmp/.t-server.%d", getpid());
	atexit(cleanup);

	fd = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
	make_sun(&sun, sock_path);
	CHECK(bind(fd, (struct sockaddr *)&sun, sizeof sun));
	CHECK(listen(fd, 2));
	return fd;
}

/* Create a new socket, connected to sock_path[] */
static int
connect_local()
{
	int fd;
	struct sockaddr_un sun;
	int flags;

	fd = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
	make_sun(&sun, sock_path);
	CHECK(connect(fd, (struct sockaddr *)&sun, sizeof sun));
	/* make it non-blocking */
	flags = CHECK(fcntl(fd, F_GETFL));
	CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK));

	return fd;
}

static void
assert_read(const char *file, int line, const char *expr, int fd,
	const char *expected, int expected_len)
{
	char buffer[expected_len];
	int len = read(fd, buffer, expected_len);
	if (len < 0) {
		fprintf(stderr, "%s:%d: %s failed\n"
			"\texpected: %.*s (len %d)\n"
			"\tactual:   -1 %s\n",
			file, line, expr,
			expected_len, expected, expected_len,
			strerror(errno));
		exit(1);
	}
	if (len != expected_len || memcmp(buffer, expected, len) != 0) {
		fprintf(stderr, "%s:%d: %s failed\n"
			"\texpected: %.*s (len %d)\n"
			"\tactual:   %.*s (len %d)\n",
			file, line, expr,
			expected_len, expected, expected_len,
			len, buffer, len);
		exit(1);
	}
}
#define ASSERT_READ(fd, s) \
	assert_read(__FILE__, __LINE__, \
		"ASSERT_READ(" #fd ", " #s ")", \
		fd, s, sizeof s - 1)
#define WRITE(fd, s) \
	assert(CHECK(write(fd, s, sizeof s - 1)) == sizeof s - 1)

/* -- mocked callbacks -- */

/* destructive counter check */
#define WAS_CALLED(m) (m.counter == 1 ? (m.counter = 0), 1 : 0)

static struct {
	unsigned int counter;
	struct server *s;
	int fd;
	void *listener;
	void *retval;
} mock_on_accept;
static void *
mock_on_accept_fn(struct server *s, int fd, void *listener)
{
	mock_on_accept.counter++;
	mock_on_accept.s = s;
	mock_on_accept.fd = fd;
	mock_on_accept.listener = listener;
	return mock_on_accept.retval;
}

static struct {
	unsigned int counter;
	struct server *s;
	void *client;
	int fd;
	int retval;
	int reterrno;
} mock_on_ready;
static int
mock_on_ready_fn(struct server *s, void *client, int fd)
{
	mock_on_ready.counter++;
	mock_on_ready.s = s;
	mock_on_ready.client = client;
	mock_on_ready.fd = fd;
	if (mock_on_ready.retval == -1)
		errno = mock_on_ready.reterrno;
	return mock_on_ready.retval;
}

static struct mock_on_close {
	unsigned int counter;
	struct server *s;
	void *client;
} mock_on_close;
static void
mock_on_close_fn(struct server *s, void *client)
{
	mock_on_close.counter++;
	mock_on_close.s = s;
	mock_on_close.client = client;
}

static struct mock_on_listener_close {
	unsigned int counter;
	struct server *s;
	void *listener;
} mock_on_listener_close;
static void
mock_on_listener_close_fn(struct server *s, void *listener)
{
	mock_on_listener_close.counter++;
	mock_on_listener_close.s = s;
	mock_on_listener_close.listener = listener;
}

static struct {
	unsigned int counter;
	struct server *s;
	char msg[2048];
} mock_on_error;
static void
mock_on_error_fn(struct server *s, const char *msg)
{
#if DEBUG
	fprintf(stderr, "on_error: %s\n", msg);
#endif
	mock_on_error.counter++;
	mock_on_error.s = s;
	snprintf(mock_on_error.msg, sizeof mock_on_error.msg, "%s", msg);
}

int
main()
{
	int listenfd;
	int xfd;			/* test-private external fd */
	int client_fd;			/* client fd as known to the server */
	struct server *server;
	struct server_context context;
	static char LISTEN[] = "LISTEN";
	static char CLIENT[] = "CLIENT";
	char discard;

	/* Have a SIGALRM cancel us if we somehow get blocked */
	assert(signal(SIGALRM, SIG_DFL) != SIG_ERR);
	alarm(1);

	/* we can initialize a server instance */
	memset(&context, 0, sizeof context);
	context.max_sockets = 0;
	context.on_accept = mock_on_accept_fn;
	context.on_ready = mock_on_ready_fn;
	context.on_close = mock_on_close_fn;
	context.on_listener_close = mock_on_listener_close_fn;
	context.on_error = mock_on_error_fn;
	server = server_new(&context);
	assert(server);

	/* Poll with no data should return 0 ready */
	assert(CHECK(server_poll(server, 0)) == 0);

	/* can attach a listener socket */
	listenfd = listen_local();
	CHECK(server_add_listener(server, listenfd, LISTEN));
	assert(CHECK(server_poll(server, 0)) == 0);

	/* Private connection to the (only) listener, to trigger an accept */
	xfd = CHECK(connect_local());

	/* After we poll the server now, we should see one fd active */
	mock_on_accept.retval = CLIENT;
	assert(CHECK(server_poll(server, 0)) == 1);
	/* the accept callback should have happened */
	assert(WAS_CALLED(mock_on_accept));
	assert(mock_on_accept.s == server);
	assert(mock_on_accept.listener == LISTEN);
	assert(mock_on_accept.fd != -1);
	assert(mock_on_accept.fd != listenfd);
	client_fd = mock_on_accept.fd;
	/* if we now send data in, we should see the on_ready callback */
	mock_on_ready.retval = 1;
	WRITE(xfd, "hello");
	assert(CHECK(server_poll(server, 0)) == 1);
	assert(WAS_CALLED(mock_on_ready));
	assert(mock_on_ready.s == server);
	assert(mock_on_ready.client == CLIENT);
	assert(mock_on_ready.fd == client_fd);
	ASSERT_READ(mock_on_ready.fd, "hello");
	/* now that the data is all drained, we should poll()=0 */
	assert(CHECK(server_poll(server, 0)) == 0);

	/* Closing the external xfd should trigger an on_ready, and
	 * if we return 0 from on_ready, that will trigger an on_close */
	CHECK(close(xfd));
	mock_on_ready.retval = 0;
	assert(CHECK(server_poll(server, 0)) == 1);
	assert(WAS_CALLED(mock_on_ready));
	assert(mock_on_ready.s == server);
	assert(mock_on_ready.client == CLIENT);
	assert(mock_on_ready.fd == client_fd);
	assert(WAS_CALLED(mock_on_close));
	assert(mock_on_close.s == server);
	assert(mock_on_close.client == CLIENT);

	/* connect xfd again, then this time we'll simulate the
	 * on_ready callback calling shutdown(SHUT_RD) on the client_fd. */
	xfd = CHECK(connect_local());
	mock_on_accept.retval = CLIENT;
	assert(CHECK(server_poll(server, 0)) == 1);
	assert(WAS_CALLED(mock_on_accept));
	assert(mock_on_accept.listener == LISTEN);
	client_fd = mock_on_accept.fd;
	assert(CHECK(server_poll(server, 0)) == 0);
	/* we can shut down the inside fd */
	CHECK(shutdown_read(client_fd));
	/* reading the inside fd shall immediately yield EOF */
	assert(CHECK(read(client_fd, &discard, sizeof discard)) == 0);
	/* returning 0 from on_ready() shall close the connection */
	mock_on_ready.retval = 0;
	assert(CHECK(server_poll(server, 0)) == 1);
	assert(WAS_CALLED(mock_on_ready));
	assert(WAS_CALLED(mock_on_close));
	CHECK(close(xfd));

	/* simulate an EIO error */
	xfd = CHECK(connect_local());
	mock_on_accept.retval = CLIENT;
	assert(CHECK(server_poll(server, 0)) == 1);
	assert(WAS_CALLED(mock_on_accept));
	client_fd = mock_on_accept.fd;
	WRITE(xfd, "hello");
	mock_on_ready.retval = -1;
	mock_on_ready.reterrno = EIO;
	assert(CHECK(server_poll(server, 0)) == 1);
	assert(WAS_CALLED(mock_on_ready));
	assert(WAS_CALLED(mock_on_close));
	assert(WAS_CALLED(mock_on_error));
	assert(strstr(mock_on_error.msg, strerror(EIO)));
	CHECK(close(xfd));

	/* closing the server closes the listeners */
	server_free(server);
	assert(WAS_CALLED(mock_on_listener_close));
	assert(mock_on_listener_close.s == server);
	assert(mock_on_listener_close.listener == LISTEN);
}
