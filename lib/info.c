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

static int fd = -1;
static char last_error[1024];
static struct proto *proto;
static int tx_begun;

static struct {
	unsigned char until_msg;	/* on arrival, increments done */
	int done;
	struct info_bind *binds;
	const char *exists_key;
	int exists_ret;
	char *buffer;
	int buflen;
	size_t buffersz;
	int (*info_cb)(const char *key, const char *value,
		unsigned int sz);
} waitret;

static void
waitret_init()
{
	memset(&waitret, 0, sizeof waitret);
}

/* Copy the value from a MSG_INFO into the info_bind, allocating
 * space from waitret.buffer. Returns -1 on error (ie ENOMEM) */
static int
waitret_info(struct info_bind *b, const char *data, unsigned int datalen)
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

/* Called from wait_until_done as messages arrive */
static int
wait_on_input(struct proto *p, unsigned char msg,
	const char *data, unsigned int datalen)
{
	if (waitret.until_msg == msg)
		waitret.done++;
	if (msg == MSG_EOF)
		return 0;
	if (msg == MSG_INFO && waitret.exists_key &&
	    strcmp(waitret.exists_key, data) == 0)
	{
		/* info_exists() */
		waitret.exists_ret = (strlen(waitret.exists_key) != datalen);
		waitret.done++;
	}
	if (msg == MSG_INFO && waitret.binds) {
		/* info_readv() */
		struct info_bind *b;
		for (b = waitret.binds; b->key; b++) {
			if (strcmp(data, b->key) == 0) {
				if (waitret_info(b, data, datalen) == -1)
					return -1;
				break;
			}
		}
	}
	if (msg == MSG_INFO && waitret.info_cb) {
		int keylen = strlen(data);
		unsigned int valuesz;
		const char *value;
		if (keylen == datalen) {
			value = NULL;
			valuesz = 0;
		} else {
			value = data + keylen + 1;
			valuesz = datalen - (keylen + 1);
		}
		if (waitret.info_cb(data, value, valuesz) == 0) {
			if (waitret.until_msg == MSG_EOF)
				waitret.done++;
		}
	}
	if (msg == MSG_ERROR) {
		snprintf(last_error, sizeof last_error,
			"(server) %.*s", datalen, data);
		return 0;
	}
	return 1;
}

/* Receive messages until the conditions within waitret are satisfied,
 * or a connection error occurs.
 * Returns 0 on MSG_ERROR, -1 on errno, or 1 on success. */
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
			return 0;
		}
		if (len == -1)
			return len;
		buf[len] = '\0';
		len = proto_recv(proto, buf, len);
		if (len <= 0)
			return len;
	}
	return waitret.done;
}

int
info_read(const char *key, char *buf, unsigned int bufsz)
{
	struct info_bind bind[2];
	bind[0].key = key;
	bind[1].key = NULL;
	if (info_readv(bind, buf, bufsz) == -1)
		return -1;
	memmove(bind[0].value, buf, bind[0].valuesz);
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

int
info_delete(const char *key)
{
	return info_write(key, NULL, 0);
}

int
info_exists(const char *key)
{
	if (info_open(NULL) == -1)
		return -1;
	if (proto_output(proto, CMD_READ, "%s", key) == -1)
		goto fail;
	waitret_init();
	waitret.exists_key = key;
	if (wait_until(MSG_EOF) <= 0)
		goto fail;
	return waitret.exists_ret;
fail:
	info_close();
	return -1;
}

int
info_readv(struct info_bind *binds, char *buffer, unsigned int buffersz)
{
	const struct info_bind *b;

	if (info_open(NULL) == -1)
		return -1;
	if (proto_output(proto, CMD_BEGIN, "") == -1)
		goto fail;
	for (b = binds; b->key; b++)
		if (proto_output(proto, CMD_READ, "%s", b->key) == -1)
			goto fail;
	if (proto_output(proto, CMD_PING, "") == -1)
		goto fail;
	if (proto_output(proto, CMD_COMMIT, "") == -1)
		goto fail;
	waitret_init();
	waitret.binds = binds;
	waitret.buffer = buffer;
	waitret.buffersz = buffersz;
	if (wait_until(MSG_PONG) <= 0)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_writev(const struct info_bind *binds)
{
	const struct info_bind *b;

	if (info_open(NULL) == -1)
		return -1;
	if (proto_output(proto, CMD_BEGIN, "") == -1)
		goto fail;
	for (b = binds; b->key; b++) {
		if (b->value) {
			if (proto_output(proto, CMD_WRITE, "%s%c%*s",
			    b->key, 0, b->valuesz, b->value) == -1)
				goto fail;
		} else {
			if (proto_output(proto, CMD_WRITE, "%s", b->key) == -1)
				goto fail;
		}
	}
	if (proto_output(proto, CMD_PING, "") == -1)
		goto fail;
	if (proto_output(proto, CMD_COMMIT, "") == -1)
		goto fail;
	waitret_init();
	if (wait_until(MSG_PONG) <= 0)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_begin()
{
	if (tx_begun) {
		errno = EIO;
		return -1;
	}
	if (info_open(NULL) == -1)
		return -1;
	if (proto_output(proto, CMD_BEGIN, "") == -1)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_tx_read(const char *key)
{
	if (!tx_begun) {
		errno = EIO;
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
		errno = EIO;
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
		errno = EIO;
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
		errno = EIO;
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
info_tx_commit(int (*cb)(const char *key, const char *value, unsigned int sz))
{
	if (!tx_begun) {
		errno = EIO;
		return -1;
	}
	if (proto_output(proto, CMD_PING, "") == -1)
		goto fail;
	if (proto_output(proto, CMD_COMMIT, "") == -1)
		goto fail;
	tx_begun = 0;
	waitret_init();
	waitret.info_cb = cb;
	if (wait_until(MSG_PONG) <= 0)
		goto fail;
	return 0;
fail:
	info_close();
	return -1;
}

int
info_sub_wait(int (*cb)(const char *key, const char *value, unsigned int sz))
{
	int ret;

	if (info_open(NULL) == -1)
		return -1;
	waitret_init();
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
info_on_sendv(struct proto *p, const struct iovec *iovs, int niovs)
{
	return writev(fd, iovs, niovs);
}

int
info_open(const char *hostport)
{
	int framed;

	if (fd != -1 && proto)
		return 0;
	if (fd == -1) {
		if (hostport) {
			fd = open_tcp(hostport);
			if (fd == -1)
				return -1;
			framed = 0;
		} else {
			fd = sockunix_connect();
			if (fd == -1) {
				snprintf(last_error, sizeof last_error,
					"sockunix_connect: %s",
					strerror(errno));
				return -1;
			}
			framed = 1;
		}
	}
	if (!proto) {
		proto = proto_new();
		if (!proto)
			return -1;
	}
	if (proto_set_mode(proto,
	    framed ? PROTO_MODE_FRAMED : PROTO_MODE_BINARY) == -1)
		goto fail;
	waitret_init();
	tx_begun = 1;
	proto_set_on_input(proto, wait_on_input);
	proto_set_on_sendv(proto, info_on_sendv);
	/* send HELLO? */
	return 0;

fail:
	info_close();
	return -1;
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

