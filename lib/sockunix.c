#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sockunix.h"

#define offsetof(T, field) ((size_t)&((T *)0)->field)

static void
init_address(struct sockaddr_un *sun, socklen_t *len_return)
{
	const char *path;
	static const char default_path[] = INFOD_SOCKET;
	int len;

	memset(sun, 0, sizeof *sun);
	sun->sun_family = AF_UNIX;

	path = getenv("INFOD_SOCKET");
	if (path && *path) {
		len = strlen(path);
		if (len > sizeof sun->sun_path)
			len = sizeof sun->sun_path;
	} else {
		path = default_path;
		len = sizeof default_path - 1;
	}
	memcpy(sun->sun_path, path, len);

	*len_return = offsetof(struct sockaddr_un, sun_path) + len;
}

int
sockunix_listen()
{
	int s;
	struct sockaddr_un sun;
	socklen_t sunlen = sizeof sun;

	s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (s == -1)
		return -1;
	init_address(&sun, &sunlen);
	if (bind(s, (struct sockaddr *)&sun, sunlen) == -1) {
		close(s);
		return -1;
	}
	if (listen(s, 5) == -1) {
		close(s);
		return -1;
	}
	return s;
}

int
sockunix_connect()
{
	int s;
	struct sockaddr_un sun;
	socklen_t sunlen = sizeof sun;

	s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (s == -1)
		return -1;
	init_address(&sun, &sunlen);
	if (connect(s, (struct sockaddr *)&sun, sunlen) == -1) {
		close(s);
		return -1;
	}
	return s;
}

const char *
sockunix_peername(int fd, char *buf, size_t sz)
{
	snprintf(buf, sz, "local/%d", fd);
	return buf;
}

