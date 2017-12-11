#pragma once

int info_read(const char *key, char *buf, unsigned int bufsz);
int info_write(const char *key, const char *value, unsigned int valuesz);
int info_delete(const char *key);
int info_exists(const char *key);

struct info_bind {
	const char *key;	/* NULL sentinel */
	char *value;		/* NULL iff deleted, const when writing */
	unsigned int valuesz;
};
/* Reads .key fields and fills in .buf and .bufsz fields */
int info_readv(struct info_bind *binds, char *buffer, unsigned int sz);
int info_writev(const struct info_bind *binds);

/* Opens socket to server. This function is called automatically
 * by other functions as needed. It will also retry failed connections
 * with a backoff delay. */
int info_open(const char *server);
extern unsigned int info_retries;
/* Closes the socket opened by info_read() etc */
void info_close(void);
int info_fileno(void);
/* Half-closes the connections. Used by timeouts to cancel an operation. */
void info_shutdown(void);

/* Transactional */
int info_tx_begin();
int info_tx_read(const char *key);
int info_tx_write(const char *key, const char *value, unsigned int valuesz);
int info_tx_delete(const char *key);
int info_tx_sub(const char *pattern);
/* Commit the transaction. The optional callback will receive all tx_read
 * and tx_sub initial results. The callback should return 1 to continue,
 * or 0 to terminate. */
int info_tx_commit(int (*cb)(const char *key,
	const char *value, unsigned int sz));

/* Wait for more updates from a subscription. If the cb returns 0 the wait
 * loop will terminate and return 0 */
int info_sub_wait(int (*cb)(const char *key,
	const char *value, unsigned int sz));

/* Returns the last error message received from the server. */
const char *info_get_last_error(void);

