#pragma once

/**
 * Reads a binary value from the info server.
 *
 * @param key    name of the value
 * @param buf    output buffer for holding the binary value
 * @param bufsz  size of @a buf
 *
 * @returns length of the binary value read in bytes
 * @retval -1 [ENOENT] The value does not exist.
 * @retval -1 [EBUSY]  Re-entrant use detected from callback.
 * @retval -1 [EPIPE]  Server error; see #info_get_last_error()
 * @retval -1 Service error, see #errno.
 *
 * This function is unsuitable for use from a callback.
 * Use #info_cb_read() instead.
 */
int info_read(const char *key, char *buf, unsigned int bufsz);

/**
 * Reads a string value from the info server.
 *
 * This is the same as #info_read(), except that it locally
 * terminates the value with a NUL to make it a C string.
 *
 * @param key    name of the value
 * @param buf    output buffer for receiving the value (and NUL)
 * @param bufsz  size of @a buf
 *
 * @returns @a buf on success
 * @retval NULL [ENOENT] The value does not exist or was deleted.
 * @retval NULL [EBUSY]  Re-entrant use detected from callback.
 * @retval NULL [ENOMEM] The buffer was too small for the value.
 * @retval NULL [EPIPE]  Server error; see #info_get_last_error()
 * @retval NULL Service error; see #errno.
 *
 * This function is unsuitable for use from a callback.
 * Use #info_cb_read() instead.
 */
char *info_reads(const char *key, char *buf, unsigned int bufsz);

/**
 * Stores or updates a value in the info server.
 *
 * @param key      name of the value
 * @param value    value data, or NULL to delete
 * @param valuesz  length of the value data, or 0 when deleting
 *
 * @retval 0 Request sent.
 * @retval -1 [ENOMEM] A value and its key exceeded 64kB in size.
 * @retval -1 Service error, see #errno.
 */
int info_write(const char *key, const char *value, unsigned int valuesz);

/**
 * Stores or updates a value in the info server.
 *
 * This is the same as <code>info_write(s, strlen(s))</code>,
 * except that @a value may be NULL to indicate a delete request.
 *
 * @param key        name of the value
 * @param value_str  string to store, or NULL to delete;
 *                   The terminating NUL will be ignored.
 *
 * @retval 0 Request sent.
 * @retval -1 [ENOMEM] A value and its key exceeded 64kB in size.
 * @retval -1 Service error, see #errno.
 */
int info_writes(const char *key, const char *value_str);

/**
 * Deletes a named value from the info server.
 *
 * @param key  name of the value to delete
 *
 * @retval 0 Request sent.
 * @retval -1 Service error, see #errno.
 */
int info_delete(const char *key);

/**
 * Tests the existence of a named value on the info server.
 *
 * @param key  name of the value to test
 *
 * @retval 0 The key has been deleted.
 * @retval 1 The key exists and has a value (which may be empty).
 * @retval -1 [EBUSY]  Re-entrant use detected from callback.
 * @retval -1 [EPIPE]  Server error; see #info_get_last_error()
 * @retval -1 Service error, see #errno.
 *
 * This function cannot be used from a callback.
 */
int info_exists(const char *key);

/**
 * Associates a key with value storage.
 *
 * Use this structure for coherent reads (or writes) of
 * multiple values. That is, reads and writes happen
 * all-at-once from any one client's point of view.
 *
 * To use, create an array of #info_bind structures,
 * fill in the keys (and values for #info_writev())
 * and terminate the array by setting its info_bind#key
 * field to NULL.
 */
struct info_bind {
	const char *key;	/**< Caller-supplied key name, or
				     NULL for sentinel entry.  */
	char *value;		/**< Caller-supplied value for #info_writev()
				     or set to storage by #info_readv().
				     NULL indicates delete/deleted. */
	unsigned int valuesz;	/**< Size of the value in bytes. */
};

/**
 * Reads a collection of value from the info server.
 *
 * This function atomically queries the info server for all
 * of the keys named in the @a binds array.
 * On success, it concatenates all of the received value data
 * into the provided @a buffer, and updates the info_bind#value
 * and info_bind#valuesz fields to reference the @a buffer.
 *
 * @param binds  An array of bindings terminated with a NULL key entry.
 *               Only the info_bind#key fields should be set.
 * @param buffer storage to use for returned values.
 *               On success the info_bind#value fields of @a binds
 *		 will either be NULL or point into the @a buffer.
 *
 * @retval 0 Success.
 * @retval -1 [ENOMEM] The buffer was too small for the request.
 * @retval -1 [EBUSY]  Re-entrant use detected from callback.
 * @retval -1 [EPIPE]  Server error; see #info_get_last_error()
 * @retval -1 Service error, see #errno.
 *
 * This function cannot be used in a callback.
 */
int info_readv(struct info_bind *binds, char *buffer, unsigned int buffersz);

/**
 * Stores a collection of values in the info server.
 *
 * This function atomically updates the info server for all
 * of the key/value pairs provided in the @a binds array.
 *
 * @param binds  An array of bindings terminated with a NULL key entry.
 *		 A info_bind#value of NULL indicates a delete request.
 *
 * @retval 0 Request sent.
 * @retval -1 Service error, see #errno.
 */
int info_writev(const struct info_bind *binds);

/**
 * Opens a connection to the infod server.
 *
 * If the connection is already open, this function has no effect.
 *
 * This function is called automatically by
 *	#info_exists(), #info_read(), #info_reads(), #info_readv(),
 *	#info_write(), #info_writes(), #info_writev(), #info_delete(),
 *	#info_tx_begin(), #info_loop()
 *
 * @param server path to the server socket, or NULL to use
 *               the environment variable INFOD_SOCKET if
 *               set, otherwise use the built-in default
 *		 <code>"\0INFOD"</code>
 *
 * @retval 0 The connection is open.
 * @revtal -1 Service error (after many retries), see #errno.
 *
 * @see  #info_retries, #info_close(), #info_fileno().
 */
int info_open(const char *server);

/**
 * The number of retries that #info_open() will attempt
 * after the first connection attempt fails.
 *
 * Between each retry, #info_open() will sleep an
 * increasingly longer time.
 * The default is quite high (100), making the worst-case
 * duration of #info_open() approximately 1.5 hours.
 */
extern unsigned int info_retries;

/**
 * Closes the connection opened by #info_open().
 *
 * This function has no effect if the connection has
 * not yet been opened.
 *
 * @retval 0 The connection is closed.
 * @retval -1 [EBUSY]  Re-entrant use detected from callback.
 *
 * @note After being closed, the connection can be
 * automatically reopened by other functions.
 *
 * This function cannot be called from a callback. Please
 * use #info_cb_close() instead.
 */
int info_close(void);

/**
 * Returns the file descriptor used for the server connection.
 * @retval -1 The connection is closed.
 */
int info_fileno(void);


/**
 * Begins a transaction group.
 * After this call, the following procedures can be called
 * in any order:
 *   #info_tx_read(),
 *   #info_tx_write(),
 *   #info_tx_delete(),
 *   #info_tx_sub(),
 * and then finalised by
 *   #info_tx_commit().
 *
 * @retval 0 Success.
 * @retval -1 Service error, see #errno.
 */
int info_tx_begin(void);

/**
 * Schedules a value read in the current transaction.
 *
 * @param key  name of the value to read
 *
 * @retval 0 Read scheduled; ready for #info_tx_commit().
 * @retval -1 [EIO] Transaction not started, see #info_tx_begin()
 * @retval -1 Service error, see #errno.
 */
int info_tx_read(const char *key);

/**
 * Schedules a value update in the current transaction.
 *
 * @param key    name of the value to update
 * @param value  value data to store, or NULL to indicate delete
 * @param value  size of data to store
 *
 * @retval 0 Write scheduled; ready for #info_tx_commit().
 * @retval -1 [EIO] Transaction not started, see #info_tx_begin()
 * @retval -1 Service error, see #errno.
 */
int info_tx_write(const char *key, const char *value, unsigned int valuesz);

/**
 * Schedules a value delete in the current transaction.
 *
 * @param key    name of the value to delete
 *
 * @retval 0 Delete scheduled; ready for #info_tx_commit().
 * @retval -1 [EIO] Transaction not started, see #info_tx_begin()
 * @retval -1 Service error, see #errno.
 */
int info_tx_delete(const char *key);

/**
 * Schedules a subscription in the current transaction.
 *
 * On commit, the server will reply immediately with all
 * matching values of the subscription. Thereafter it will
 * provide updates on key updates that match the @a pattern.
 *
 * @param pattern   key subscription pattern
 *
 * The @a pattern is a UTF-8 string with some characters ("metchars")
 * having special meaning:
 * <pre>( | ) * ? \ </pre>
 * The @a pattern is matched from left-to-right against each candidate key,
 * succeding when the pattern matches an entire key.
 *
 * The elements of a pattern are:
 * <dl>
 * <dt><i>c</i></dt>
 * <dd>Any regular (non-metachar) character <i>c</i> matches itself.</dd>
 * <dt><code>?</code></dt>
 * <dd>Matches any single character.</dd>
 * <dt><code>*</code><i>c</i></dt>
 * <dd>Matches the shortest substring (including empty string) up to and
 *     then including the next regular character <i>c</i>.</dd>
 * <dt><code>*</code></dt>
 * <dd>A <code>*</code> at the end of a pattern or followed by <code>|</code>
 *     or <code>)</code> greedily matches the rest of the key string.
 *     Patterns containing <code>**</code> or <code>*(</code> are invalid.
 *     The pattern <code>*?</code> is equivalent to <code>?</code>.</dt>
 * <dt><code>(</code><i>x</i><code>|</code><i>y</i><code>|</code>...<code>)</code></dt>
 * <dd>A group find the first matching branch of <i>x</i> or <i>y</i> etc.
 *     Once a branch is matched, subsequent branches are ignored and
 *     matching continues after the closing <code>)</code>.
 *     Parenthesised groups can be nested up to four deep.</dd>
 * <dt><code>\</code><i>c</i></dt>
 * <dd>Matches exactly the character <i>c</i> which may be a metachar.</dd>
 * </dl>
 *
 * The greedy nature of <code>*</code> means that the pattern
 * <code>iface.*.mtu</code> will match the key <code>iface.eth0.mtu</code>
 * but not <code>iface.bridge0.port1.mtu</code>.
 * However, the pattern <code>iface.*</code> will match both.
 *
 * @retval 0 Subscription scheduled; ready for #info_tx_commit().
 * @retval -1 [EIO] Transaction not started, see #info_tx_begin()
 * @retval -1 Service error, see #errno.
 */
int info_tx_sub(const char *pattern);

/**
 * Schedules an unsubscription in the current transaction.
 *
 * Requests that the server unsubscribe from the pattern.
 * This can be used to perform a single, bulk query of all
 * keys that match a given pattern.
 *
 * @param pattern   key pattern to unsubscribe from
 *
 * @retval 0 Unsubscription scheduled; ready for #info_tx_commit().
 * @retval -1 [EIO] Transaction not started, see #info_tx_begin()
 * @retval -1 Service error, see #errno.
 */
int info_tx_unsub(const char *pattern);

/**
 * Callback function type.
 *
 * This function is called on receipt of a key-value update message
 * from the info server, usually triggered by one of
 * #info_tx_sub(), #info_tx_read(), #info_cb_read(), #info_cb_sub(),
 *
 * @param key      name of the value in the update message
 * @param value    new value, or NULL to indicate a deletion.
 * @param valuesz  length of the new value
 *
 * @retval -1 Terminate the invoking #info_tx_commit(), #info_loop()
 *            function. #errno will be passed unchanged to their caller.
 * @retval 0  Successful handling. Terminate the invoking #info_loop().
 * @retval >0 Message was handled; caller should continue waiting for
 *            more messages.
 */
typedef int (*info_cb_fn)(const char *key, const char *value, unsigned int valuesz);

/**
 * Commits the current transaction and waits for the immediate results.
 *
 * This function appends a PING request to the current transaction, and
 * then commits it with a COMMIT message. Then, whlie it waits for the
 * matching PONG reply, it invokes the callback functoin @a cb for
 * other messages received and they are the immediate effects of the
 * transaction.
 *
 * @param cb callback function to handle the "immediate" transaction
 *           replies
 *
 * @retval 0 The transaction was completed.
 * @retval -1 [EIO] Transaction not started, see #info_tx_begin()
 * @retval -1 The callback function @a cb returned -1
 * @retval -1 Service error, see #errno.
 */
int info_tx_commit(info_cb_fn cb);

/**
 * Wait for and handle updates from the info server.
 *
 * This function loops while the @a cb function returns
 * a positive number (usually 1).
 *
 * @param cb callback function for info server messages.
 *           It should returns 1 to continue, or 0 to
 *           terminate this loop.
 *
 * @retval 0  The @a cb function returned 0.
 * @retval -1 The @a cb function returned a negative number.
 *            #errno was preserved.
 * @retval -1 [EBADF] The server connection is closed.
 * @retval -1 Service error, see #errno
 */
int info_loop(info_cb_fn cb);

/**
 * Reads one message from the connection and dispatches
 * it to the callback function.
 *
 * This function can be called when #poll() or #select()
 * indicates a ready-for-read condition on FD #info_fileno().
 *
 * On error the caller should check if #info_fileno() returns -1.
 * This indicates the connection was closed. In this condition,
 * the caller must also arrange for #info_open() to be called
 * and completed before being able to call #info_recv1() again.
 *
 * @param cb callback function for info server messages.
 *           It should return a positive integer on success.
 *
 * @retval -1 The @a cb function returned a negative number.
 *            #errno was preserved.
 * @retval -1 [EBADF] The server connection is closed.
 * @retval -1 Service error, see #errno
 */
int info_recv1(info_cb_fn cb);

/**
 * Requests a value read of the server from within a callback.
 *
 * @param key  name of the value
 * @retval -1 Service error; see #errno.
 * @retval otherwise Request sent.
 *
 * This function can be called from within a callback function
 * invoked by #info_loop().
 * The callback should eventually received the requested value.
 */
int info_cb_read(const char *key);

/**
 * Requests adding another subscription from within a callback.
 *
 * @param pattern   key subscription pattern
 *
 * @retval -1 Service error; see #errno.
 * @retval otherwise Request sent.
 *
 * @see #info_tx_sub()
 */
int info_cb_sub(const char *pattern);

/**
 * Requests removing a prior subscription from within a callback.
 *
 * @param pattern   key pattern to unsubscribe from
 *
 * @retval -1 Service error; see #errno.
 * @retval otherwise Request sent.
 *
 * @see #info_tx_sub()
 */
int info_cb_unsub(const char *pattern);

/**
 * Requests closing the server connection from within a callback.
 */
void info_cb_close(void);

/**
 * Returns the last error message.
 *
 * When EPIPE errors are returned, this function will return
 * a human-readable explanation.
 *
 * @returns pointer to static storage holding the last error message
 */
const char *info_get_last_error(void);

