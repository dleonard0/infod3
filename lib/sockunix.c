#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sockunix.h"

static void
init_address(struct sockaddr_un *sun)
{
	char *path;
	static const char default_path[] = INFOD_SOCKET;

	memset(sun, 0, sizeof *sun);
	sun->sun_family = AF_UNIX;

	path = getenv(INFOD_SOCKET);
	if (path && *path)
		snprintf(sun->sun_path, sizeof sun->sun_path, "%s",
		     path);
	else
		memcpy(sun->sun_path, default_path, sizeof default_path);
}

int
sockunix_listen()
{
	int s;
	struct sockaddr_un sun;

	s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (s == -1)
		return -1;
	init_address(&sun);
	if (bind(s, (struct sockaddr *)&sun, sizeof sun) == -1) {
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

	s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (s == -1)
		return -1;
	init_address(&sun);
	if (connect(s, (struct sockaddr *)&sun, sizeof sun) == -1) {
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

