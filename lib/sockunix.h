#pragma once

/* Environment variable used for socket path */
#define INFOD_SOCKET "\0INFOD"

int sockunix_listen(void);
int sockunix_connect(void);
const char *sockunix_peername(int fd, char *buf, size_t sz);

