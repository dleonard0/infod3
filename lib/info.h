#pragma once

/* Reads the value of a key from the info server.
 * This function cannot be used from a callback (see info_cb_read).
 * Returns the length of the value read (the length may be 0).
 * Returns 0 if the key has been deleted.
 * Returns -1 on error. */
int info_read(const char *key, char *buf, unsigned int bufsz);

/* Stores a key and its new value.
 * If value is NULL, this is the same as deleting the key.
 * Returns -1 on error. */
int info_write(const char *key, const char *value, unsigned int valuesz);

/* Deletes a key.
 * Returns -1 on error. */
int info_delete(const char *key);

/* Tests for a key's existence
 * This function cannot be used in a callback.
 * Returns 0 if the key has been deleted.
 * Returns 1 if the key has a value (which may be empty).
 * Returns -1 on error. */
int info_exists(const char *key);


/* For multiple reads (or writes), you can perform
 * "coherent" operations on the remote store where
 * coherent writes and coherent reads are never split from the
 * client's point of view.
 * A 'binding' structure is a set of key names with associated values
 * that will be filled in or used by info_readv() and info_writev()
 * as a single unit. */
struct info_bind {
	const char *key;	/* NULL means this is a sentinel */
	char *value;		/* NULL means deleted */
	unsigned int valuesz;
};

/* Reads key-values from the server into bindings.
 * All values received are concatenated into the supplied buffer,
 * and then pointers and spans to the values are stored in the binding
 * array.
 * This function cannot be used in a callback.
 *
 * @a binds    an array of bindings terminated with a NULL key
 * @a buffer   the buffer to use for value storage.
 *
 * Returns -1 on error.  */
int info_readv(struct info_bind *binds, char *buffer, unsigned int buffersz);

/* Stores key-value bindings in the info server.
 * A binding with a NULL value indicates a delete operation.
 * Returns -1 on error. */
int info_writev(const struct info_bind *binds);


/* Opens a connection to the infod server if not already open.
 * If NULL is passed, use the default unix socket.
 * Note: This function is called automatically and so need never be
 * called explicitly. */
int info_open(const char *server);

/* Limit to the number of retries that info_open() will attempt. */
extern unsigned int info_retries;

/* Closes the connection opened by info_open().
 * Note that the connection may be automatically reopened. */
void info_close(void);

/* Returns the file descriptor used for the server connection.
 * Returns -1 if the connection is closed. */
int info_fileno(void);

/* Begins a transaction group.
 * After this call, the following procedures can be called:
 *   info_tx_read()
 *   info_tx_write()
 *   info_tx_delete()
 *   info_tx_sub()
 * Finally, call info_tx_commit() to enact the queued operations.
 */
int info_tx_begin();

/* Schedues a read in the current transaction. */
int info_tx_read(const char *key);

/* Schedues a write in the current transaction. */
int info_tx_write(const char *key, const char *value, unsigned int valuesz);

/* Schedues a delete in the current transaction. */
int info_tx_delete(const char *key);

/* Schedues a subscription in the current transaction. */
int info_tx_sub(const char *pattern);

/* Schedues an unsubscription in the current transaction. */
int info_tx_unsub(const char *pattern);

/* Commits the transaction to complete, then waits for immediate results
 * (i.e. explicit reads and the implicit initial reads from subscriptions).
 * Returns 0 if all transactions completed.
 * Returns -1 immediately if the callback function returns -1.
 * Returns -1 on error and sets errno. */
int info_tx_commit(int (*cb)(const char *key,
	const char *value, unsigned int sz));

/* Wait for more updates from a subscription prepared by info_tx_sub().
 * This function loops while the cb function returns >0.
 * If the cb function returns <=0 then this function will immediately
 * return that value. */
int info_sub_wait(int (*cb)(const char *key,
	const char *value, unsigned int sz));

/* Initiate a read for a key.
 * This can be used in a callback, especially during info_sub_wait().
 * Returns -1 on error and sets errno. */
int info_cb_read(const char *key);
/* Adds another subscription for the current callback.
 * Returns -1 on error and sets errno. */
int info_cb_sub(const char *pattern);
/* Removes a previous subscription.
 * Returns -1 on error and sets errno. */
int info_cb_unsub(const char *pattern);

/* Half-closes the current infod server connections.
 * This operation can be called during callbacks to commence
 * closing a connection while avoiding a file descriptor race.
 * Error handling code elsewhere will fully close the connection. */
void info_cb_close(void);

/* Returns the last error message received from the server. */
const char *info_get_last_error(void);

