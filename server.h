
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

struct server *server_new(const struct server_context *c);

/* Closes all clients and deallocates */
void server_free(struct server *server);

/* Adds a listener FD to the server. Only accept() will be called on this.
 * The fd will only be closed by server_free().
 * Returns -1 on error. */
int server_add_listener(struct server *server, int fd, void *listener);

/* Dispatch all pending I/O just once, possibly blocking.
 * Call this multiple times in a loop.
 * A timeout of -1 blocks forever.
 * Returns what poll() returned. */
int server_poll(struct server *server, int timeout);

/* Shut down the read side of a FD, in order to trigger an on_ready()
 * callback. This is preferred over a blunt close() which can introduce
 * file descriptor re-use races. Instead, a half-closed FD will cause
 * on_ready() to be called, and read() will return -1.
 */
int shutdown_read(int fd);
