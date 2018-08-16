/*
 * A low-memory key-value server.
 * Clients may subscribe to changes.
 * Client transactions permit coherent views.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <syslog.h>
#include <netdb.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "server.h"
#include "../lib/proto.h"
#include "../lib/sockunix.h"
#include "../lib/socktcp.h"
#include "storepath.h"
#include "store.h"
#include "match.h"
#include "list.h"

#define MAX_SUBS	16		/* Maximum subscriptions per client */
#define MAX_BUFCMDS	32		/* Maximum cmds in a transaction */

static struct options {
#ifndef SMALL
	unsigned char verbose;		/* -v */
# define VERBOSE options.verbose
	unsigned char stdin;		/* -i */
	const char *port;		/* -p */
#else
/* VERBOSE is a constant so that branches can be optimized away */
# define VERBOSE 0
#endif
	unsigned char syslog;		/* -s */
	const char *store_path;		/* -f */
} options;

/* global store */
static struct store *the_store;

/* pre-framed unix listener */
static struct listener unix_listener = { "unix", NULL };

/* Client connection record */
struct client {
	LINK(struct client);
	int fd;			/* accepted socket */
	struct proto *proto;	/* protocol state */

	unsigned int nsubs;
	unsigned int nbufcmds;
	unsigned int begins;

	/* Active subscripotions */
	struct subscription {
		LINK(struct subscription);
		unsigned int pattern_len;
		char pattern[];	/* pattern for match() */
	} *subs;

	/* A buffered command held during unclosed BEGIN */
	struct bufcmd {
		LINK(struct bufcmd);
		unsigned int datalen;
		unsigned char msg;
		char data[];
	} *bufcmds, **bufcmd_tail;

#ifndef SMALL
	struct listener *listener; /* only used for verbose logs */
#endif
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

	return client;
}

/* Find the subscription record for the client, or NULL */
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

/* This is called just after a client's fd is closed */
static void
on_net_close(struct server *s, void *c, struct listener *l)
{
	struct client *client = c;
	if (VERBOSE) {
		char namebuf[PEERNAMESZ];
		log_msgf(LOG_INFO, "[%s] closed",
			listener_peername(l, client->fd,
				namebuf, sizeof namebuf));
	}
	REMOVE(client);
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
	char buf[PROTO_RECVSZ + 1];
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

/* Tests if data[] contains NUL; ie could not be a C string */
static int
contains_nul(const char *data, unsigned int datalen)
{
	return !!memchr(data, '\0', datalen);
}

/* Tests if data[] contains '!' followed by the first NUL;
 * That is the key ends with '!'. */
static int
is_ephemeral(const char *data, unsigned int datalen)
{
	const char *nul = memchr(data, '\0', datalen);
	return nul && nul > data && nul[-1] == '!';
}

/* Buffer a message received after CMD_BEGIN.
 * The message is stored in the bufcmds list attached
 * to the client record.
 * Later, the corresponing CMD_COMMIT will trigger
 * execution of all buffered commands. */
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
	APPEND(bcmd, &client->bufcmd_tail);
	return 1;
}

#ifndef SMALL
static void
log_input_verbose(struct client *client, unsigned char msg,
	const char *data, unsigned int datalen)
{
	char namebuf[PEERNAMESZ];
	char p[PEERNAMESZ+2];

	snprintf(p, sizeof p, "[%s] got%c", listener_peername(client->listener,
		client->fd, namebuf, sizeof namebuf),
		client->begins ? '+' : ' ');

	switch (msg) {
#define L(...) log_msgf(LOG_DEBUG, __VA_ARGS__)
	case CMD_HELLO:
	  switch (datalen) {
	  case 0: L("%s HELLO", p); break;
	  default: L("%s HELLO %u %.*s", p, data[0] & 0xff, datalen-1, data+1);
	  } break;
	case CMD_SUB: L("%s SUB %.*s", p, datalen, data); break;
	case CMD_UNSUB: L("%s UNSUB %.*s", p, datalen, data); break;
	case CMD_READ: L("%s READ %.*s", p, datalen, data); break;
	case CMD_WRITE: {
	  unsigned int kl = strlen(data);
	  if (kl == datalen) L("%s WRITE %.*s (delete)", p, datalen, data);
	  else L("%s WRITE %s %.*s", p, data, datalen-kl-1, data+kl+1);
	  } break;
	case CMD_BEGIN: L("%s BEGIN %.*s", p, datalen, data); break;
	case CMD_COMMIT: L("%s COMMIT %.*s", p, datalen, data); break;
	case CMD_PING: L("%s PING %.*s", p, datalen, data); break;
	case MSG_EOF: L("%s <EOF>", p); break;
	default: L("%s <msg=%02x,len=%u> %.*s", p, msg, datalen, datalen,data);
#undef L
	}
}
#endif

/* This is called when a protocol message has been decoded
 * from the client. That is, we've received a valid
 * command message from the client. */
static int
on_app_input(struct proto *p, unsigned char msg,
	const char *data, unsigned int datalen)
{
	struct client *client = proto_get_udata(p);
	struct client *c;
	struct subscription *sub;
	const struct info *info;
	struct store_index ix;

#ifndef SMALL
	if (VERBOSE > 1)
		log_input_verbose(client, msg, data, datalen);
#endif

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
		INSERT(sub, &client->subs);
		client->nsubs++;
		for (info = store_get_first(the_store, &ix);
		     info;
		     info = store_get_next(the_store, &ix))
		{
			if (match(data, info->keyvalue))
				if (proto_output(p, MSG_INFO, "%*s",
				    info->sz, info->keyvalue) == -1)
					return -1;
		}
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
		return info ? proto_output(p, MSG_INFO, "%*s", info->sz,
				    info->keyvalue)
			    : proto_output(p, MSG_INFO, "%s", data);
	case CMD_WRITE:
		if (!contains_nul(data, datalen)) {
			/* Delete */
			int ret = store_del(the_store, data);
			if (ret == 0)
				return 1; /* del had no effect */
			if (ret == -1)
				return proto_output_error(p,
					PROTO_ERROR_INTERNAL, "del: %s",
					strerror(errno));
		} else if (is_ephemeral(data, datalen)) {
			/* Put key!\0value */
		} else {
			/* Put */
			int ret = store_put(the_store, datalen, data);
			if (ret == 0)
				return 1; /* put had no effect */
			if (ret == -1)
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
					{
#ifndef SMALL
						char namebuf[PEERNAMESZ];
						log_msgf(LOG_ERR,
						    "[%s] dropped: %m",
						    listener_peername(
						    c->listener, c->fd,
						    namebuf, sizeof namebuf));
#endif
						(void)shutdown_read(c->fd);
					}
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


/* This is called when the listener socket has accepted a
 * new connection, and the new connection is being added
 * to the server's poll list.
 * Allocate and attach a new client record to it. */
static void *
on_net_accept(struct server *s, int fd, struct listener *l)
{
	struct client *client;
	char namebuf[PEERNAMESZ];

	if (VERBOSE)
		log_msgf(LOG_INFO, "[%s] connected",
			listener_peername(l, fd, namebuf, sizeof namebuf));

	client = client_new(fd);
	if (!client) { /* failed to allocate */
		const char *estr = strerror(errno);
		log_msgf(LOG_WARNING, "[%s] client_new(): %s",
			listener_peername(l, fd, namebuf, sizeof namebuf),
			estr);
		shutdown_read(fd);
		return NULL;
	}

	INSERT(client, &all_clients);
#ifndef SMALL
	client->listener = l;
#endif
	if (l == &unix_listener)
		proto_set_mode(client->proto, PROTO_MODE_FRAMED);

	proto_set_on_error(client->proto, on_proto_error);
	proto_set_on_sendv(client->proto, on_net_sendv);
	proto_set_on_input(client->proto, on_app_input);

	return client;
}

/* basename() is not always available in libc? */
#define basename portable_basename
static const char *
basename(const char *name)
{
	const char *s = strrchr(name, '/');
	return s ? s + 1 : name;
}

static void
add_unix_listener(struct server *server)
{
	int fd = sockunix_listen();

	if (server_add_listener(server, fd, &unix_listener) == -1) {
		log_perror("unix listener");
		exit(1);
	}
}

#ifndef SMALL
static void
add_stdin_listener(struct server *server)
{
	static struct listener stdin_listener = { "stdin", NULL };

	if (server_add_fd(server, STDIN_FILENO, &stdin_listener) == -1) {
		log_perror("stdin listener");
		exit(1);
	}
}

static void
add_tcp_listeners(struct server *server)
{
	static struct listener tcp_listener = { "tcp", tcp_peername };
	struct addrinfo *ais = NULL;
	struct addrinfo *ai;
	int count = 0;
	int ret;

	ret = tcp_server_addrinfo(options.port, &ais);
	if (ret != 0) {
		log_msgf(LOG_ERR, "tcp_server_addrinfo: %s",
			gai_strerror(ret));
		exit(1);
	}
	for (ai = ais; ai; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype,
			ai->ai_protocol);
		if (fd == -1) {
			log_msgf(LOG_ERR, "socket: %s", strerror(errno));
			continue;
		}

#ifdef __linux__
		/* getaddrinfo() returns AF_INET and AF_INET6 addresses
		 * for the same port, but Linux shares TCP port space
		 * between IPv4 and v6 by default, so these collide.
		 * Defeat this with IPV6_V6ONLY */
		if (ai->ai_family == AF_INET6) {
			int val = 1;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
				&val, sizeof val) == -1)
			    log_msgf(LOG_WARNING, "IPV6_V6ONLY: %s",
				strerror(errno));;
		}
#endif

		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
			log_msgf(LOG_ERR, "bind: %s", strerror(errno));
			close(fd);
			continue;
		}
		if (listen(fd, 5) == -1) {
			log_msgf(LOG_ERR, "listen: %s", strerror(errno));
			close(fd);
			continue;
		}
		if (server_add_listener(server, fd, &tcp_listener) == -1) {
			log_perror("tcp listener");
			close(fd);
			continue;
		}
		count++;
	}
	freeaddrinfo(ais);
	if (!count)
		exit(1);
}
#endif /* !SMALL */

static int terminated;	/* True when a SIGTERM was received */
static void
on_sigterm(int sig)
{
	terminated = 1;
}

int
main(int argc, char *argv[])
{
	struct server_context server_context;
	struct server *server;
	int ret;
	int error = 0;
	int ch;
	static const char *option_flags =
		"f:"
		"s"
#ifndef SMALL
		"p:"
		"i"
		"v"
#endif /* !SMALL */
		;
	options.store_path = STORE_PATH;

	while ((ch = getopt(argc, argv, option_flags)) != -1)
		switch (ch) {
		case 'f':
			options.store_path = optarg;
			break;
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

	if (optind < argc)
		error = 2; /* excess arguments */
	if (error) {
		if (error == 2) {
			fprintf(stderr, "usage: %s"
#ifdef SMALL
						" [-s]"
#else /* !SMALL */
						" [-siv] [-p port]"
#endif /* !SMALL */
						" [-f db]"
				"\n",
				argv[0]);
		}
		exit(error);
	}

	if (options.syslog)
		openlog(basename(argv[0]), LOG_CONS | LOG_PERROR, LOG_DAEMON);

	the_store = store_open(options.store_path);
	if (!the_store) {
		log_msgf(LOG_ERR, "store_open: %s: %s", options.store_path,
			strerror(errno));
		exit(1);
	}

	memset(&server_context, 0, sizeof server_context);
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
	add_tcp_listeners(server);
#endif /* !SMALL */
	add_unix_listener(server);

	/* handle clean termination signals */
	if (signal(SIGTERM, on_sigterm) == SIG_ERR) {
		log_perror("signal SIGTERM");
		exit(1);
	}
	if (signal(SIGINT, on_sigterm) == SIG_ERR) {
		log_perror("signal SIGINT");
		exit(1);
	}

	/* main loop */
	while ((ret = server_poll(server, -1)) > 0) {
		if (ret == -1) {
			if (!(errno == EINTR && terminated))
				log_perror("poll");
		}
		if (terminated)
			break;
	}
	if (ret == 0 && VERBOSE)
		on_net_error(server, "no listeners!");

	if (!terminated || VERBOSE)
		log_msg(LOG_ERR, "terminating");
	server_free(server);
	store_close(the_store);
	exit(terminated);
}
