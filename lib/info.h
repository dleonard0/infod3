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

/* Optional; this function will be automatically called
 * if the connection is not already open */
int info_open(const char *server);
/* Closes the socket opened by info_read() etc */
void info_close(void);
int info_fileno(void);

/* Transactional */
int info_tx_begin();
int info_tx_read(const char *key);
int info_tx_write(const char *key, const char *value, unsigned int valuesz);
int info_tx_delete(const char *key);
int info_tx_sub(const char *pattern);
/* Commit the transaction. The callback will be called for all reads, and
 * the immediate subscription patterns */
int info_tx_commit(int (*cb)(const char *key, const char *value, unsigned int sz));

/* Wait for more updates from a subscription. If the cb returns 0 the wait
 * will return 0 */
int info_sub_wait(int (*cb)(const char *key, const char *value, unsigned int sz));

/* Returns the last error message received */
const char *info_get_last_error(void);

