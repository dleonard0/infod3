/*
 * Infod3 server
 *  - serves a key-value set
 *  - informes subscribed clients of key updates
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <syslog.h>

#include <sys/uio.h>

#include "server.h"
#include "../lib/proto.h"
#include "store.h"
#include "match.h"
#include "list.h"

#define MAX_SUBS	16
#define MAX_BUFCMDS	32

static struct options {
#ifndef SMALL
	unsigned char verbose;
# define VERBOSE options.verbose
	unsigned char stdin;
	const char *port;
#else /* VERBOSE is a constant so that branches can be optimized away */
# define VERBOSE 0
#endif
	unsigned char syslog;
} options;

/* Single store */
static struct store *the_store;

/* Client connections */
struct client {
	LINK(struct client);
	int fd;
	struct proto *proto;

	unsigned int nsubs;
	unsigned int nbufcmds;
	unsigned int begins;

	/* A subscription is a pattern for match() */
	struct subscription {
		LINK(struct subscription);
		unsigned int pattern_len;
		char pattern[];
	} *subs;

	/* A buffered command */
	struct bufcmd {
		LINK(struct bufcmd);
		unsigned int datalen;
		unsigned char msg;
		char data[];
	} *bufcmds, **bufcmd_tail;

} *all_clients;

static int on_app_input(struct proto *p, unsigned char msg,
	 const char *data, unsigned int datalen);

static void
log_msg(int level, const char *msg)
{
	if (options.syslog)
		syslog(level, "%s", msg);
	fprintf(stderr, "%s\n", msg);
}

static void
log_msgf(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (options.syslog) {
		va_list ap2;
		va_copy(ap2, ap);
		vsyslog(level, fmt, ap2);
		va_end(ap2);
	}
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void
log_perror(const char *msg)
{
	const char *estr = strerror(errno);
	if (options.syslog)
		syslog(LOG_ERR, "%s: %s", msg, estr);
	fprintf(stderr, "%s: %s\n", msg, estr);
}

struct subscription *
subscription_new(const char *pattern, unsigned int pattern_len)
{
	struct subscription *sub = malloc(sizeof *sub + pattern_len + 1);
	if (sub) {
		sub->pattern_len = pattern_len;
		memcpy(sub->pattern, pattern, pattern_len);
		sub->pattern[pattern_len] = '\0';
	}
	return sub;
}

static void
subscription_free(struct subscription *sub)
{
	free(sub);
}


struct bufcmd *
bufcmd_new(unsigned char msg, const char *data, unsigned int datalen)
{
	struct bufcmd *bcmd = malloc(sizeof *bcmd + datalen + 1);
	if (bcmd) {
		bcmd->datalen = datalen;
		bcmd->msg = msg;
		memcpy(bcmd->data, data, datalen + 1);
	}
	return bcmd;
}

static void
bufcmd_free(struct bufcmd *bcmd)
{
	free(bcmd);
}

static void
client_free(struct client *client)
{
	struct subscription *sub;
	struct bufcmd *bcmd;

	proto_free(client->proto);

	while ((sub = client->subs)) {
		REMOVE(sub);
		subscription_free(sub);
	}
	while ((bcmd = client->bufcmds)) {
		REMOVE(bcmd);
		bufcmd_free(bcmd);
	}

	free(client);
}

static struct client *
client_new(int fd)
{
	struct client *client;
	struct proto *proto;

	client = malloc(sizeof *client);
	if (!client)
		return NULL;

	proto = proto_new();
	if (!proto) {
		free(client);
		return NULL;
	}

	client->proto = proto;
	client->fd = fd;
	client->subs = NULL;
	client->nsubs = 0;
	client->begins = 0;
	client->bufcmds = NULL;
	client->nbufcmds = 0;

	/* We don't use a udata free function, because
	 * the client owns the proto, not vice versa. */
	proto_set_udata(proto, client, NULL);

	INSERT(client, all_clients);
	return client;
}

static struct subscription *
client_find_subscription(struct client *client, const char *pattern,
	unsigned int pattern_len)
{
	struct subscription *sub;
	for (sub = client->subs; sub; sub = NEXT(sub))
		if (sub->pattern_len == pattern_len &&
		    memcmp(pattern, sub->pattern, pattern_len) == 0)
			break;
	return sub;
}

static void
on_net_close(struct server *s, void *c, struct listener *l)
{
	struct client *client = c;
	client_free(client);
}

static void
on_proto_error(struct proto *p, const char *msg)
{
	log_msg(LOG_INFO, msg);
}

static void
on_net_error(struct server *s, const char *msg)
{
	log_msg(LOG_WARNING, msg);
}

static int
on_net_ready(struct server *s, void *c, int fd)
{
	/* Read network data into a buffer on the stack, and
	 * deliver the buffer to the protocol decoder. */
	struct client *client = c;
	char buf[65536 + 1];
	int len = read(fd, buf, sizeof buf - 1);
	if (len < 0)
		return -1;
	buf[len] = '\0';
	return proto_recv(client->proto, buf, len);
}

static int
on_net_sendv(struct proto *p, const struct iovec *iovs, int niovs)
{
	/* Pass protocol network output straight to socket */
	struct client *client = proto_get_udata(p);
	if (!client)
		return 0;
	/* If there is no buffer space available, this will
	 * return -1/EAGAIN and the server will drop
	 * the connection. */
	return writev(client->fd, iovs, niovs);
}

/* Tests if the data could not be a C string, ie contains a NUL */
static int
contains_nul(const char *data, unsigned int datalen)
{
	return !!memchr(data, '\0', datalen);
}

/* Handle a message received after CMD_BEGIN.
 * A balanced CMD_COMMIT will re-execute all buffered commands. */
static int
buffer_command(struct client *client, unsigned char msg,
	const char *data, unsigned int datalen)
{
	struct bufcmd *bcmd;
	int ret;
	struct proto *p = client->proto;

	if (msg == CMD_BEGIN) {	/* push a nested BEGIN */
		++client->begins;
		return 1;
	}
	if (msg == CMD_COMMIT) {
		if (--client->begins) /* pop a nested BEGIN */
			return 1;
		/* playback all the buffered cmds in order */
		while ((bcmd = client->bufcmds)) {
			REMOVE(bcmd);
			ret = on_app_input(p, bcmd->msg, bcmd->data,
				bcmd->datalen);
			bufcmd_free(bcmd);
			if (ret <= 0)
				return ret;
		}
		return 1;
	}
	if (client->nbufcmds >= MAX_BUFCMDS)
		return proto_output_error(p, PROTO_ERROR_TOO_BIG,
			"commit buffer overflow");
	bcmd = bufcmd_new(msg, data, datalen);
	if (!bcmd)
		return proto_output_error(p, PROTO_ERROR_INTERNAL,
			"begin: %s", strerror(errno));
	APPEND(bcmd, client->bufcmd_tail);
	return 1;
}

static int
on_app_input(struct proto *p, unsigned char msg,
	 const char *data, unsigned int datalen)
{
	struct client *client = proto_get_udata(p);
	struct client *c;
	struct subscription *sub;
	struct info *info;
	struct index *index;
	int ret;

	if (msg == MSG_EOF)
		return 0;

	if (client->begins)
		return buffer_command(client, msg, data, datalen);

	switch (msg) {
	case CMD_HELLO:
		return proto_output(p, MSG_VERSION, "%c%s", 0, "infod3");
	case CMD_SUB:
		if (client->nsubs > MAX_SUBS)
			return proto_output_error(p, PROTO_ERROR_TOO_BIG,
				"sub: too many subscriptions");
		if (contains_nul(data, datalen) || !match_isvalid(data))
			return proto_output_error(p, PROTO_ERROR_BAD_ARG,
				"sub: invalid pattern");
		sub = subscription_new(data, datalen);
		if (!sub)
			return proto_output_error(p, PROTO_ERROR_INTERNAL,
				"sub: %s", strerror(errno));
		INSERT(sub, client->subs);
		client->nsubs++;
		index = index_open(the_store);
		if (!index)
			return proto_output_error(p, PROTO_ERROR_INTERNAL,
				"sub: %s", strerror(errno));
		while ((info = index_next(index)))
			if (match(data, info->keyvalue))
				if (proto_output(p, MSG_INFO, "%*s",
				    info->sz, info->keyvalue) == -1)
					return -1;
		index_close(index);
		return 1;
	case CMD_UNSUB:
		sub = client_find_subscription(client, data, datalen);
		if (!sub)
			return 1;
		REMOVE(sub);
		client->nsubs--;
		subscription_free(sub);
		return 1;
	case CMD_READ:
		if (contains_nul(data, datalen))
			return proto_output_error(p, PROTO_ERROR_BAD_ARG,
				"read: invalid key");
		info = store_get(the_store, data);
		if (!info)
			return proto_output(p, MSG_INFO, "%s%c", data, 0);
		ret = proto_output(p, MSG_INFO, "%*s", info->sz,
			info->keyvalue);
		info_decref(info);
		return ret;
	case CMD_WRITE:
		if (!contains_nul(data, datalen)) {
			/* delete check if already deleted */
			info = store_get(the_store, data);
			if (!info)
				return 1;
			store_del(the_store, info);
			info_decref(info);
		} else {
			/* check if same value already */
			info = store_get(the_store, data);
			info_decref(info);
			if (info &&
			    info->sz == datalen &&
			    memcmp(data, info->keyvalue, datalen) == 0)
				return 1; /* no change */
			info = info_new(datalen);
			if (!info)
				return proto_output_error(p,
					PROTO_ERROR_INTERNAL, "write: %s",
					strerror(errno));
			memcpy(info->keyvalue, data, datalen);
			if (store_put(the_store, info) == -1)
				return proto_output_error(p,
					PROTO_ERROR_INTERNAL, "write: %s",
					strerror(errno));
		}
		/* notify all subscribers */
		for (c = all_clients; c; c = NEXT(c))
			for (sub = c->subs; sub; sub = NEXT(sub))
				if (match(sub->pattern, data))
					if (proto_output(c->proto, MSG_INFO,
					    "%*s", datalen, data) == -1)
						(void)shutdown_read(c->fd);
		return 1;
	case CMD_PING:
		return proto_output(p, MSG_PONG, "%*s", datalen, data);
	case CMD_BEGIN:
		client->bufcmd_tail = &client->bufcmds;
		client->begins = 1;
		return 1;
	case CMD_COMMIT:
		return proto_output_error(p, PROTO_ERROR_BAD_SEQ,
			"commit: no begin");
	default:
		return proto_output_error(p, PROTO_ERROR_BAD_MSG,
			"unexpected message %02x", msg);
	}
}


/* The listener has accepted a new fd.
 * Attach a new client context to it */
static void *
on_net_accept(struct server *s, int fd, struct listener *l)
{
	struct client *client;

	client = client_new(fd);
	if (!client) { /* failed to allocate */
		char namebuf[256];
		const char *estr = strerror(errno);
		log_msgf(LOG_WARNING, "[%s] client_new(): %s",
			l ? l->peername(fd, namebuf, sizeof namebuf) : "?",
			estr);
		shutdown_read(fd);
		return NULL;
	}
	proto_set_on_error(client->proto, on_proto_error);
	proto_set_on_sendv(client->proto, on_net_sendv);
	proto_set_on_input(client->proto, on_app_input);
	return client;
}

/* basename not always available in libc */
#define basename portable_basename
static const char *
basename(const char *name)
{
	const char *s = strrchr(name, '/');
	return s ? s + 1 : name;
}

#ifndef SMALL
/* A fake listener context for the stdin fd */
static const char *
stdin_peername(int fd, char *buf, size_t sz)
{
	return "stdin";
}
static struct listener stdin_listener = { "stdin", stdin_peername };
#endif

int
main(int argc, char *argv[])
{
	struct server_context server_context;
	struct server *server;
	int ret;
	int error = 0;
	int ch;
	static const char *option_flags =
		"s"
#ifndef SMALL
		"p:"
		"i"
		"v"
#endif /* !SMALL */
		;

	while ((ch = getopt(argc, argv, option_flags)) != -1)
		switch (ch) {
		case 's':
			options.syslog = 1;
			break;
#ifndef SMALL
		case 'p':
			options.port = optarg;
			break;
		case 'i':
			options.stdin = 1;
			break;
		case 'v':
			VERBOSE++;
			break;
#endif /* !SMALL */
		case '?':
			error = 2;
			break;
		}
	if (error) {
		if (error == 2) {
			fprintf(stderr, "usage: %s "
#ifndef SMALL
						"[-s]"
#else /* SMALL */
						"[-siv] [-p port]"
#endif /* SMALL */
				, argv[0]);
		}
		server_free(server);
		exit(error);
	}

	if (options.syslog)
		openlog(basename(argv[0]), LOG_CONS | LOG_PERROR, LOG_DAEMON);

	the_store = store_new();
	if (!the_store) {
		log_perror("store_new");
		exit(1);
	}

	server_context.max_sockets = 64;
	server_context.on_accept = on_net_accept;
	server_context.on_ready = on_net_ready;
	server_context.on_close = on_net_close;
	server_context.on_error = on_net_error;

	server = server_new(&server_context);
	if (!server) {
		log_perror("server_new");
		exit(1);
	}

#ifndef SMALL
	if (options.stdin)
		add_stdin_listener(server);
#endif /* !SMALL */

	while ((ret = server_poll(server, -1)) > 0)
		if (ret == -1)
			log_perror("poll");
	if (ret == 0 && VERBOSE)
		on_net_error(server, "no listeners!");

	server_free(server);
	store_free(the_store);
}
