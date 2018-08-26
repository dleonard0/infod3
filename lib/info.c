
/*
 * A simple C API for info client applications.
 *    - global state (not thread safe)
 *    - automatic connection to the server
 *    - simple synchronous operations by default
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "info.h"
#include "proto.h"
#include "socktcp.h"
#include "sockunix.h"

static int fd = -1;			/* connection to server */
static char last_error[1024];
static struct proto *proto;
static int tx_begun;

unsigned int info_retries = 100;

/* a wait-until descriptor */
static struct {
	unsigned char until_msg;	/* rxing this msg increments .done */
	int in_cb;			/* true while inside .info_cb() */
	int done;			/* inc'd when .until_msg received */
	struct info_bind *binds;
	const char *exists_key;
	int exists_ret;
	char *buffer;
	int buflen;
	size_t buffersz;
	int (*info_cb)(const char *key, const char *value,
		unsigned int sz);
} waitret;

/* Operations that are constrained to callbacks */
static int
cb_op(unsigned int msg, const char *arg)
{
#if 0 /* Do not enforce just yet */
	if (!waitret.in_cb) {
		errno = EINVAL;
		return -1;
	}
#endif
	return proto_output(proto, msg, "%s", arg);
}

int
info_cb_read(const char *key)
{
	return cb_op(CMD_READ, key);
}

int
info_cb_sub(const char *pattern)
{
	return cb_op(CMD_SUB, pattern);
}

int
info_cb_unsub(const char *pattern)
{
	return cb_op(CMD_UNSUB, pattern);
}

/* Initializes the waitret status.
 * Returns -1 (EBUSY) if the waitret's callback is active,
 * which means reentrant use of waitret was attempted */
static int
waitret_init()
{
	if (waitret.in_cb) {
		errno = EBUSY;
		return -1;
	}
	memset(&waitret, 0, sizeof waitret);
	return 0;
}

/* Fills in a variable binding using the key\0value from data[],
 * and allocates storage from waitret.buffer.
 * Returns -1 on ENOMEM, 0 on success. */
static int
waitret_bind(struct info_bind *b, const char *data, unsigned int datalen)
{
	int valuesz;
	int keylen = strlen(data);
	const char *value;

	if (keylen == datalen) { /* Deleted value */
		b->value = NULL;
		b->valuesz = 0;
		return 0;
	}

	value = data + keylen + 1;
	valuesz = datalen - (keylen + 1);
	if (valuesz + waitret.buflen > waitret.buffersz) {
		errno = ENOMEM;
		return -1;
	}
	b->value = waitret.buffer + waitret.buflen;
	b->valuesz = valuesz;
	memcpy(b->value, value, valuesz);
	waitret.buflen += valuesz;
	return 0;
}

/* This procedure is indirectly called from wait_until().
 * It handles each received message according to the settings
 * in the global waitret. Its main job is to set waitret.done
 * when it receives a message with code equal to waitret.until_msg. */
static int
on_input(struct proto *p, unsigned char msg,
	const char *data, unsigned int datalen)
{
	if (waitret.until_msg == msg)
		waitret.done++;
	if (msg == MSG_EOF) {
		snprintf(last_error, sizeof last_error, "Connection closed");
		return 0;
	}
	if (msg == MSG_INFO && waitret.exists_key &&
	    strcmp(waitret.exists_key, data) == 0)
	{
		/* called from info_exists() */
		waitret.exists_ret = (strlen(waitret.exists_key) != datalen);
		waitret.done++;
	}
	if (msg == MSG_INFO && waitret.binds) {
		/* called from info_readv() */
		struct info_bind *b;
		for (b = waitret.binds; b->key; b++) {
			if (strcmp(data, b->key) == 0) {
				if (waitret_bind(b, data, datalen) == -1)
					return -1;
				break;
			}
		}
	}
	if (msg == MSG_INFO && waitret.info_cb) {
		/* called from info_tx_commit() / info_sub_wait() */
		int keylen = strlen(data);
		unsigned int valuesz;
		const char *value;
		int cb_ret = 0;
		/* Prepare the INFO message for info_cb() */
		if (keylen == datalen) {
			value = NULL;
			valuesz = 0;
		} else {
			value = data + keylen + 1;
			valuesz = datalen - (keylen + 1);
		}
		waitret.in_cb = 1;
		cb_ret = waitret.info_cb(data, value, valuesz);
		waitret.in_cb = 0;
		if (cb_ret == 0 && waitret.until_msg == MSG_EOF) {
			/* The callback function for info_sub_wait()
			 * returned 0, which we'll interpret to mean
			 * "that's enough". */
			waitret.done++;
		}
		if (cb_ret == -1)
			return -1;
	}
	if (msg == MSG_ERROR) {
		/* Network protocol error */
		snprintf(last_error, sizeof last_error,
			"(server) %.*s", datalen, data);
		return 0;
	}
	return 1;
}

/* Receive messages until waitret.done is set by on_input(),
 * or until a connection error occurs.
 * Returns -1 on errno,
 * Returns 1+ on success. */
static int
wait_until(unsigned char msg)
{
	char buf[PROTO_RECVSZ + 1];
	int len;

	waitret.until_msg = msg;
	waitret.done = 0;
	while (!waitret.done) {
		len = read(fd, buf, sizeof buf - 1);
		if (len == 0) {
			snprintf(last_error, sizeof last_error,
				"connection closed by server");
			goto eof;
		}
		if (len == -1) {
			snprintf(last_error, sizeof last_error,
				"read: %s", strerror(errno));
			return -1;
		}
		/* assert(len <= sizeof buf - 1); because of read() */
		buf[len] = '\0';
		len = proto_recv(proto, buf, len);
		if (len == 0)
			goto eof;
		if (len < 0)
			return len;
	}
	return waitret.done;
eof:
	/* Convert early EOF into some semblance of an error */
	errno = EPIPE;
	return -1;
}

int
info_read(const char *key, char *buf, unsigned int bufsz)
{
	struct info_bind bind[2];
	bind[0].key = key;
	bind[1].key = NULL;
	if (info_readv(bind, buf, bufsz) == -1)
		return -1;
	if (bind[0].value == NULL) {
		errno = ENOENT;
		return -1;
	}
	memmove(buf, bind[0].value, bind[0].valuesz);
	return bind[0].valuesz;
}

int
info_write(const char *key, const char *value, unsigned int valuesz)
{
	struct info_bind bind[2];
	bind[0].key = key;
	bind[0].value = (char *)value;
	bind[0].valuesz = valuesz;
	bind[1].key = NULL;
	return info_writev(bind);
}

char *
info_reads(const char *key, char *buf, unsigned int bufsz)
{
	struct info_bind bind[2];
	char *ret;

	/* Require space in buf for the trailing \0 */
	if (bufsz == 0) {
		errno = ENOMEM;
		return NULL;
	}

	bind[0].key = key;
	bind[1].key = NULL;
	if (info_readv(bind, buf, bufsz - 1) == -1)
		return NULL;
	if (!bind[0].value) {
		errno = ENOENT;
		return NULL;
	}
	ret = bind[0].value;
	ret[bind[0].valuesz] = '\0';
	return ret;
}

int
info_writes(const char *key, const char *value_str)
{
	return info_write(key, value_str, value_str ? strlen(value_str) : 0);
}

int
info_delete(const char *key)
{
	return info_write(key, NULL, 0);
}

int
info_exists(const char *key)
{
	if (waitret_init() == -1)
		return -1;
	if (info_open(NULL) == -1)
		return -1;
	if (proto_output(proto, CMD_READ, "%s", key) == -1)
		goto fail;
	waitret.exists_key = key;
	if (wait_until(MSG_EOF) == -1)
		goto fail;
	return waitret.exists_ret;
fail:
	info_close();
	return -1;
}

int
info_readv(struct info_bind *binds, char *buffer, unsigned int buffersz)
{
	struct info_bind *b;

	if (!binds[0].key)
		return 0;	/* nothing to read */
	/* Clear the results */
	for (b = binds; b->key; b++) {
		b->value = NULL;
		b->valuesz = 0;
	}
	if (waitret_init() == -1)
		return -1;
	if (info_open(NULL) == -1)
		return -1;
	if (!binds[1].key) {
		/* Just one key */
		if (proto_output(proto, CMD_READ, "%s", binds[0].key) == -1)
			goto fail;
		waitret.binds = binds;
		waitret.buffer = buffer;
		waitret.buffersz = buffersz;
		if (wait_until(MSG_INFO) == -1)
			goto fail;
	} else {
		/* Multiple keys use a transaction */
		if (proto_output(proto, CMD_BEGIN, "") == -1)
			goto fail;
		for (b = binds; b->key; b++)
			if (proto_output(proto, CMD_READ, "%s", b->key) == -1)
				goto fail;
		if (proto_output(proto, CMD_PING, "") == -1)
			goto fail;
		if (proto_output(proto, CMD_COMMIT, "") == -1)
			goto fail;
		waitret.binds = binds;
		waitret.buffer = buffer;
		waitret.buffersz = buffersz;
		if (wait_until(MSG_PONG) == -1)
			goto fail;
	}
	return 0;
fail:
	info_close();
	return -1;
}

int
info_writev(const struct info_bind *binds)
{
	const struct info_bind *b;
	int multi;

	if (!binds[0].key)
		return 0;	/* nothing to write */
	if (info_open(NULL) == -1)
		return -1;
	multi = (binds[1].key != NULL);
	if (multi) {
		if (proto_output(proto, CMD_BEGIN, "") == -1)
			goto fail;
	}
	for (b = binds; b->key; b++) {
		if (b->value) {
			if (proto_output(proto, CMD_WRITE, "%s%c%*s",
			    b->key, 0, b->valuesz, b->value) == -1)
				goto fail;
		} else {
			if (proto_output(proto, CMD_WRITE, "%s",
			    b->key) == -1)
				goto fail;
		}
	}
	if (multi) {
		if (proto_output(proto, CMD_COMMIT, "") == -1)
			goto fail;
	}
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_begin()
{
	if (tx_begun) {
		errno = EIO;	/* already in a transaction */
		return -1;
	}
	if (info_open(NULL) == -1)
		return -1;
	if (proto_output(proto, CMD_BEGIN, "") == -1)
		goto fail;
	tx_begun = 1;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_read(const char *key)
{
	if (!tx_begun) {
		errno = EIO;	/* not in a transaction */
		return -1;
	}
	if (proto_output(proto, CMD_READ, "%s", key) == -1)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_write(const char *key, const char *value, unsigned int valuesz)
{
	if (!tx_begun) {
		errno = EIO;	/* not in a transaction */
		return -1;
	}
	if (proto_output(proto, CMD_WRITE, "%s%c%*s",
	    key, 0, valuesz, value) == -1)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_delete(const char *key)
{
	if (!tx_begun) {
		errno = EIO;	/* not in a transaction */
		return -1;
	}
	if (proto_output(proto, CMD_WRITE, "%s", key) == -1)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_sub(const char *pattern)
{
	if (!tx_begun) {
		errno = EIO;	/* not in a transaction */
		return -1;
	}
	if (proto_output(proto, CMD_SUB, "%s", pattern) == -1)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_unsub(const char *pattern)
{
	if (!tx_begun) {
		errno = EIO;	/* not in a transaction */
		return -1;
	}
	if (proto_output(proto, CMD_UNSUB, "%s", pattern) == -1)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_commit(int (*cb)(const char *key, const char *value, unsigned int sz))
{
	int ret;

	if (!tx_begun) {
		errno = EIO;	/* not in a transaction */
		return -1;
	}
	if (waitret_init() == -1)
		return -1;
	if (proto_output(proto, CMD_PING, "") == -1)
		goto fail;
	if (proto_output(proto, CMD_COMMIT, "") == -1)
		goto fail;
	tx_begun = 0;
	waitret.info_cb = cb;
	ret = wait_until(MSG_PONG);
	if (ret == -1)
		goto fail;
	return ret;
fail:
	info_close();
	return -1;
}

int
info_sub_wait(int (*cb)(const char *key, const char *value, unsigned int sz))
{
	int ret;

	if (waitret_init() == -1)
		return -1;
	if (info_open(NULL) == -1)
		return -1;
	waitret.info_cb = cb;
	ret = wait_until(MSG_EOF);
	if (ret == -1)
		info_close();
	return ret;
}

static int
open_tcp(const char *hostport)
{
#ifdef SMALL
	errno = ENOTSUP;
	return -1;
#else
	struct addrinfo *ais = NULL;
	struct addrinfo *ai;
	const char *reason = "?";
	int ret;
	int s = -1;

	ret = tcp_client_addrinfo(hostport, &ais);
	if (ret != 0) {
		snprintf(last_error, sizeof last_error, "%s: %s",
			hostport ? hostport : "localhost:" INFOD3_PORT,
			gai_strerror(ret));
		return -1;
	}
	for (ai = ais; ai; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype,
			ai->ai_protocol);
		if (s == -1) {
			reason = "socket";
			continue;
		}
		if (connect(s, ai->ai_addr, ai->ai_addrlen) == -1) {
			reason = "connect";
			close(s);
			s = -1;
			continue;
		}
	}
	freeaddrinfo(ais);
	if (s == -1) {
		snprintf(last_error, sizeof last_error, "%s %s: %s",
			reason,
			hostport ? hostport : "localhost:" INFOD3_PORT,
			strerror(errno));
		return -1;
	}
	return s;
#endif /* !SMALL */
}

static int
on_sendv(struct proto *p, const struct iovec *iovs, int niovs)
{
	/* Connect the send path directly to the fd */
	return writev(fd, iovs, niovs);
}

/* Return -1 on error, 0 on success */
static int
try_connect(const char *hostport, int *mode)
{
	if (hostport) {
		fd = open_tcp(hostport);
		if (fd == -1)
			return -1;
		*mode = PROTO_MODE_BINARY;
		return 0;
	} else {
		fd = sockunix_connect();
		if (fd == -1) {
			snprintf(last_error, sizeof last_error,
				"sockunix_connect: %s",
				strerror(errno));
			return -1;
		}
		*mode = PROTO_MODE_FRAMED;
		return 0;
	}
}

int
info_open(const char *hostport)
{
	unsigned int retry;
	int mode = PROTO_MODE_UNKNOWN;

	if (fd != -1 && proto)
		return 0;

	if (waitret_init() == -1)
		return -1;

	last_error[0] = '\0';

	info_close();
	if (try_connect(hostport, &mode) == -1) {
		for (retry = 0; retry < info_retries; retry++) {
			(void) sleep(retry);
			if (try_connect(hostport, &mode) == 0)
				break;
		}
	}
	if (fd == -1)
		return -1; /* too many retries */

	proto = proto_new();
	if (!proto) {
		info_close();
		return -1;
	}

	proto_set_mode(proto, mode);
	proto_set_on_input(proto, on_input);
	proto_set_on_sendv(proto, on_sendv);
	/* send HELLO? */
	return 0;
}

void
info_close()
{
	if (proto) {
		proto_free(proto);
		proto = NULL;
	}
	if (fd != -1) {
		(void) close(fd);
		fd = -1;
	}
	tx_begun = 0;
}

void
info_cb_close()
{
	if (fd != -1)
		shutdown(fd, SHUT_RD);
}

int
info_fileno()
{
	return fd;
}

const char *
info_get_last_error()
{
	return last_error;
}

