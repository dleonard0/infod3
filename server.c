#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/socket.h>

#include "server.h"

#define TCP_PORT	26990			/* 'in' */
#define PATH_SOCKET	"/tmp/infod3.socket"
#define INCREMENT	16

struct server {
	const struct server_context *context;
	unsigned int n;				/* active connections */
	unsigned int nmax;			/* sz of socket[], pollfd[] */
	struct server_socket {
		void *data;
		int is_listener;
	} *socket;
	struct pollfd *pollfd;			/* parallel to socket[] */
};

/* Log an error. Returns -1. */
__attribute__((format(printf, 2, 3)))
static int
on_error(struct server *server, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	if (!server->context->on_error) {
		fprintf(stderr, "error: ");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	} else {
		vsnprintf(buf, sizeof buf, fmt, ap);
		server->context->on_error(server, buf);
	}
	va_end(ap);
	return -1;
}

static int
is_listener(const struct server *server, int i)
{
	// assert(i < sockets.n);
	return server->socket[i].is_listener;
}

/* Enable/disable all listening sockets.
 * This is used when we have MAX_SOCKETS open and want
 * to hold off accepting any new connections. */
static void
server_listen_enable(struct server *server, int enable)
{
	unsigned int i;
	for (i = 0; i < server->n; i++) {
		if (is_listener(server, i)) {
			struct pollfd *pollfd = &server->pollfd[i];
			if (enable) {
				pollfd->events = POLLIN;
			} else {
				pollfd->revents = 0;
				pollfd->events = 0;
			}
		}
	}
}

/* Resize server tables to fit n connections. */
static int
server_resize(struct server *server, unsigned int n)
{
	struct server_socket *new_socket;
	struct pollfd *new_pollfd;

	assert(server->n <= n);
	/* round n up to the next multiple of INCREMENT */
	if (n % INCREMENT)
		n += INCREMENT - (n % INCREMENT);
	/* Don't resize if same size, or one step smaller (hysteresis) */
	if ((server->nmax == n) || (server->nmax - 1 == n))
		return 0;

	/* Assign early in case of realloc failure */
	if (n < server->nmax)
		server->nmax = n;
	new_socket = realloc(server->socket,
		n * sizeof *new_socket);
	if (n && !new_socket)
		return on_error(server, "realloc %zu: %s",
			n * sizeof *new_socket, strerror(errno));
	server->socket = new_socket;

	new_pollfd = realloc(server->pollfd,
		n * sizeof *new_pollfd);
	if (n && !new_pollfd)
		return on_error(server, "realloc %zu: %s",
			n * sizeof *new_pollfd, strerror(errno));
	server->pollfd = new_pollfd;

	server->nmax = n;

	return 0;
}

/* adds a new <fd> to the list of sockets.
 * Returns the index on success.
 * Returns -1 on allocation failure. */
static int
server_add_socket(struct server *server, int fd)
{
	struct server_socket *socket;
	unsigned int i;
	int val;
	unsigned int max_sockets = server->context->max_sockets;

	i = server->n;
	// assert(i < server->context->max_sockets);

	if (server_resize(server, server->n + 1) == -1)
		return -1;

	/* Set the socket to non-blocking mode.
	 * The socket will have SO_SNDBUF of buffer space
	 * (which on Linux defaults to about 100kB)
	 * A full buffer is announced when write() returns EAGAIN.
	 * We can call shutdown(SHUT_RD) to trigger a race-free close. */
	val = fcntl(fd, F_GETFL);
	if (val != -1 && !(val & O_NONBLOCK))
		(void) fcntl(fd, F_SETFL, val | O_NONBLOCK);

	server->pollfd[i].fd = fd;
	server->pollfd[i].events = POLLIN;
	server->pollfd[i].revents = 0;
	socket = &server->socket[i];
	memset(socket, 0, sizeof *socket);

	server->n++;
	if (max_sockets && server->n >= max_sockets)
		server_listen_enable(server, 0);

	return i;
}

/* closes and frees the <fd,proto> at index i */
static void
server_del_socket(struct server *server, unsigned int i)
{
	unsigned int last = server->n - 1;
	struct server_socket *socket = &server->socket[i];
	unsigned int max_sockets = server->context->max_sockets;

	assert(!is_listener(server, i));

	if (close(server->pollfd[i].fd) == -1)
		on_error(server, "close: %s", strerror(errno));
	if (server->context->on_close)
		server->context->on_close(server, socket->data);

	/* Maintain the list packing */
	if (i < last) {
		server->pollfd[i] = server->pollfd[last];
		server->socket[i] = server->socket[last];
	}

	/* Note: this should be the only place that decrements
	 * server->n otherwise the enable/disable logic will break */
	server->n--;
	if (max_sockets && server->n == max_sockets - 1)
		server_listen_enable(server, 1);

	(void) server_resize(server, server->n);
}

/* accept a new connection and create a new socket */
static void
server_accept(struct server *server, int listen_fd, void *listener_data)
{
	int fd;

	fd = accept(listen_fd, NULL, NULL);
	if (fd == -1) {
		on_error(server, "accept: %s", strerror(errno));
		return;
	}

	if (server_add_fd(server, fd, listener_data) == -1) {
		if (close(fd) == -1)
			on_error(server, "server_add_fd: %s", strerror(errno));
	}
}

int
server_add_fd(struct server *server, int fd, void *listener_data)
{
	int i;
	void *data;

	i = server_add_socket(server, fd);
	if (i == -1)
		return -1;

	if (server->context->on_accept) {
		data = server->context->on_accept(server, fd, listener_data);
		/* Anything may have happened in upcall; scan for fd */
		for (i = 0; i < server->n; i++)
			if (server->pollfd[i].fd == fd) {
				server->socket[i].data = data;
				break;
			}
	}
	return 0;
}

int
server_poll(struct server *server, int timeout)
{
	int ret;
	int len;
	int revents;
	unsigned int i;

	if (!server->n)
		return 0;

	/* The revents are kept zero elsewhere */
	/* for (i = 0; i < server->n; i++) server->pollfd[i].revents = 0; */

	ret = poll(server->pollfd, server->n, timeout);
	if (ret <= 0)
		return ret;

	i = 0;
	while (i < server->n) {
		revents = server->pollfd[i].revents;
		if (!revents) { /* quiet */
			i++;
			continue;
		}

		/* clear revents for next time */
		server->pollfd[i].revents = 0;

		/* handle connection on a listner socket */
		if (is_listener(server, i)) {
			server_accept(server, server->pollfd[i].fd,
				server->socket[i].data);
			i++;
			continue;
		}

		/* handle ready data. */
		len = server->context->on_ready(server,
			server->socket[i].data, server->pollfd[i].fd);
		if (len > 0)
			i++;
		else {
			if (len == -1)
				on_error(server, "read: %s", strerror(errno));
			server_del_socket(server, i);
		}
	}
	return ret;
}

struct server *
server_new(const struct server_context *c)
{
	struct server *server = malloc(sizeof *server);
	if (server) {
		server->context = c;
		server->n = 0;
		server->nmax = 0;
		server->socket = NULL;
		server->pollfd = NULL;
	}
	return server;
}

void
server_free(struct server *server)
{
	unsigned int i;

	if (!server)
		return;

	/* Close all the non-listeners */
	for (i = 0; i < server->n; i++) {
		if (!is_listener(server, i)) {
			close(server->pollfd[i].fd);
			if (server->context->on_close)
				server->context->on_close(server,
					server->socket[i].data);
		}
	}

	/* Close all the listeners */
	for (i = 0; i < server->n; i++) {
		if (is_listener(server, i)) {
			close(server->pollfd[i].fd);
			if (server->context->on_listener_close)
				server->context->on_listener_close(server,
					server->socket[i].data);
		}
	}

	free(server->socket);
	free(server->pollfd);
	free(server);
}

int
server_add_listener(struct server *server, int fd, void *listener)
{
	int i;

	i = server_add_socket(server, fd);
	if (i < 0)
		return -1;
	server->socket[i].is_listener = 1;
	server->socket[i].data = listener;
	return 0;
}

int
shutdown_read(int fd)
{
	return shutdown(fd, SHUT_RD);
}
