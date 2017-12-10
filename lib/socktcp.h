#pragma once

struct addrinfo;

#define INFOD3_PORT "26931"		/* 'i3' */
#define INFOD_SERVER "INFOD_SERVER"

int tcp_server_addrinfo(const char *port, struct addrinfo **res);
int tcp_client_addrinfo(const char *hostport, struct addrinfo **res);
const char *tcp_peername(int fd, char *buf, size_t sz);


