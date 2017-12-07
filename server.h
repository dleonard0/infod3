#pragma once

/*
 * poll-based socket server
 * - Only knows how to poll(), accept() and close() file descriptors.
 * - Sets all accepted FDs to non-blocking.
 * - Makes upcalls to handlers, which should read() and write().
 * - Limits the number of active connections by ignoring
 *   listener sockets when socket limit is reached.
 */
struct server;

struct server_context {
	/* Limit to the number of open sockets. 0 means no limit. */
	unsigned int max_sockets;

	/* New client callback [optional].
	 * The fd will not change for the life of the client.
	 * Callback returns a new client context pointer.
	 * If callback is NULL, a NULL client context will be used. */
	void *(*on_accept)(struct server *s, int fd, void *listener);
	/* Data ready callback (for clients).
	 * This required callback is invoked when a client fd
	 * becomes ready for read (poll's POLLIN event selector).
	 * The callback should never close the fd.
	 * Callback should return 0 to close the client.
	 * Callback should return -1 to log errno and close the client.  */
	int (*on_ready)(struct server *s, void *client, int fd);
	/* Client close callback [optional].
	 * Called after on_ready() or server_free() calls close(fd).
	 * This callback matches on_accept() and can be used to
	 * deallocate the client context pointer. */
	void (*on_close)(struct server *s, void *client);
	/* Listener close callback [optional]
	 * This callback matches server_add_listener(), and can be
	 * used to deallocate a listener context. All non-listeners
	 * connections will have been closed at this point, and so
	 * will the listener socket. */
	void (*on_listener_close)(struct server *s, void *listener);
	/* Error callback callback [optional]
	 * Called when on_ready() returns -1, or when other internal
	 * allocations fail.
	 * If callback is NULL, messages are written to stderr. */
	void (*on_error)(struct server *s, const char *msg);
};

/* Creates a new server instance with no client or listener sockets. */
struct server *server_new(const struct server_context *c);

/* Adds a listener FD to the server.
 * When it becomes ready for read, accept() will be called on fd and
 * the resulting new socket will be added to the server.
 * Returns -1 on error. (ENOMEM) */
int server_add_listener(struct server *server, int fd, void *listener);

/* Adds an established connection to the server.
 * On success, the server will manage the fd's lifetime.
 * Returns -1 on error.
 */
int server_add_fd(struct server *server, int fd, void *listener);

/* Dispatch all pending I/O just once, possibly blocking.
 * Call this multiple times in a loop.
 * A timeout of -1 blocks forever. See poll().
 * Returns 0 if there are no FDs, otherwise what poll() returns. */
int server_poll(struct server *server, int timeout);

/* Shut down the read side of a FD.
 * This should be used outside of on_ready() to trigger a future on_ready()
 * callback on the FD. Inside of that on_ready(), a read() will return 0,
 * and when 0 is returned to server_poll() it will perform an orderly
 * close. This is preferred way to close a managed client file descriptor
 * because invoking an uncontrolled close() will introduce file descriptor
 * re-use races.
 * Returns -1 on error (eg EBADF). See shutdown(2)
 */
int shutdown_read(int fd);

/* Closes all client sockets, listeners and deallocates resources */
void server_free(struct server *server);
