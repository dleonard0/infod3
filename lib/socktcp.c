#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "socktcp.h"

#ifndef SMALL

int
tcp_server_addrinfo(const char *port, struct addrinfo **res)
{
	struct addrinfo hints;

	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags |= AI_PASSIVE;
	return getaddrinfo(NULL, port ? port : INFOD3_PORT, &hints, res);
}

int
tcp_client_addrinfo(const char *hostport, struct addrinfo **res)
{
	char node[256];
	struct addrinfo hints;
	const char *colon;

	if (!hostport) {
		hostport = getenv(INFOD_SERVER);
		if (!*hostport)
			hostport = NULL;
	}
	if (!hostport)
		hostport = INFOD3_PORT;

	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;

	colon = strrchr(hostport, ':');
	if (colon)
		snprintf(node, sizeof node, "%.*s", (int)(colon - hostport),
			hostport);
	return getaddrinfo(
		colon ? node : NULL,
		colon ? colon + 1 : hostport,
		&hints, res);
}

const char *
tcp_peername(int fd, char *buf, size_t sz)
{
	struct sockaddr_storage ss;
	socklen_t sslen = sizeof ss;
	char service[32];
	int hostlen;

	if (getpeername(fd, (struct sockaddr *)&ss, &sslen) == -1)
		return "n/a";
	if (getnameinfo((const struct sockaddr *)&ss, sslen,
		    buf, sz, service, sizeof service,
		    NI_NUMERICHOST | NI_NUMERICSERV) == -1)
		return "n/a";
	hostlen = strlen(buf);
	snprintf(buf + hostlen, sz - hostlen, ":%s", service);
	return buf;
}
#endif /* SMALL */
