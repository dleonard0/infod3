#pragma once
#include <stdlib.h>

struct rxbuf {
	char *buf;
	size_t max;
	size_t len;
} rx;

void rxbuf_init(struct rxbuf *rx);
void rxbuf_free(struct rxbuf *rx);
int  rxbuf_resize(struct rxbuf *rx, size_t sz);
int  rxbuf_addc(struct rxbuf *rx, char ch);
int  rxbuf_add(struct rxbuf *rx, const void *p, size_t n);
/* Clears the buffer, and ensures it has space for sz chars */
int  rxbuf_clear(struct rxbuf *rx, size_t sz);
void rxbuf_trimspace(struct rxbuf *rx);
/* Ensure a NUL follows the active buffer region */
int  rxbuf_zeropad(struct rxbuf *rx);

