#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "rxbuf.h"

/* The increment that rxbuf grows by */
#define SIZECHUNK 1024

static size_t
roundup(size_t n, size_t align)
{
	size_t remainder = n % align;
	return remainder ? n + (align - remainder) : n;
}

void
rxbuf_init(struct rxbuf *rx)
{
	memset(rx, 0, sizeof *rx);
}

void
rxbuf_free(struct rxbuf *rx)
{
	free(rx->buf);
}

/* Resize the rxbuf in increments of SIZECHUNK */
int
rxbuf_resize(struct rxbuf *rx, size_t sz)
{
	char *newbuf;

	if (sz >= 0x10000) {
		errno = ENOSPC;
		return -1;
	}
	/* Round up sz to next SIZECHUNK */
	sz = roundup(sz, SIZECHUNK);
	assert(sz >= rx->len);
	if (rx->max == sz)
		return 0;

	newbuf = realloc(rx->buf, sz);
	if (!newbuf)
		return -1;
	rx->max = sz;
	rx->buf = newbuf;
	return 0;
}

int
rxbuf_addc(struct rxbuf *rx, char ch)
{
	if ((rx->len >= rx->max) && rxbuf_resize(rx, rx->len + 1) == -1)
		return -1;
	rx->buf[rx->len++] = ch;
	return 0;
}

int
rxbuf_add(struct rxbuf *rx, const void *p, size_t n)
{
	if (rxbuf_resize(rx, rx->len + n) == -1)
		return -1;
	memcpy(rx->buf + rx->len, p, n);
	rx->len += n;
	return 0;
}

/* Clears the buffer, and ensures it has space for sz chars */
int
rxbuf_clear(struct rxbuf *rx, size_t sz)
{
	rx->len = 0;
	return rxbuf_resize(rx, sz);
}

void
rxbuf_trimspace(struct rxbuf *rx)
{
	while (rx->len && rx->buf[rx->len - 1] == ' ')
		--rx->len;
}

int
rxbuf_zeropad(struct rxbuf *rx)
{
	if (rxbuf_add(rx, "", 1) == -1)
		return -1;
	--rx->len;
	return 0;
}

