#include <errno.h>

#include <sys/uio.h>

#include "proto.h"
#include "protopriv.h"

#ifndef SMALL

/* -- binary decode -- */

/* Returns the length field (bytes 1 and 2) from the buffer,
 * or returns ~0 if not available */
static uint16_t
binary_pkt_len(struct rxbuf *rx)
{
	if (rx->len < 3)
		return ~0;
	return (rx->buf[1] & 0xff) << 8 |
	       (rx->buf[2] & 0xff) << 0;
}

int
recv_binary(struct proto *p, const char *net, unsigned int netlen)
{
	int ret = 0;

	if (!netlen) {
		if (p->on_input)
			ret =p->on_input(p, MSG_EOF, NULL, 0);
		return ret;
	}

	while (netlen) {
		unsigned int take;
		uint16_t sz = binary_pkt_len(&p->rx);

		if (p->rx.len < 3) {
			if (rxbuf_resize(&p->rx, 3) == -1)
				return -1;
			take = 3 - p->rx.len;
		} else {
			take = 3 + sz - p->rx.len;
			if (rxbuf_resize(&p->rx, 3 + sz) == -1)
				return -1;
		}

		if (take > netlen)
			take = netlen;
		rxbuf_add(&p->rx, net, take);
		net += take;
		netlen -= take;
		ret += take;

		if (p->rx.len >= 3 &&
		    p->rx.len == 3 + (sz = binary_pkt_len(&p->rx)))
		{
			if (p->on_input) {
				int n;
				if (rxbuf_zeropad(&p->rx) == -1)
					return -1;
				n = p->on_input(p, p->rx.buf[0] & 0xff,
					p->rx.buf + 3, sz);
				if (n <= 0)
					return n;
				ret += n;
			}
			if (rxbuf_clear(&p->rx, 3) == -1)
				return -1;
		}
	}
	return ret;
}


/* -- binary encode -- */

static size_t
iov_size(const struct iovec *iov, int n)
{
	const struct iovec *i;
	size_t sz = 0;

	for (i = iov; n--; i++)
		sz += i->iov_len;
	return sz;
}

int
output_binary(struct proto *p, unsigned char msg, const char *fmt,
	va_list ap)
{
	struct iovec iov[10];
	char work[16];
	char tl[3];
	size_t sz;

	int niov = to_binary_iov(p, &iov[1], ARRAY_SIZE(iov) - 1,
		work, sizeof work, fmt, ap);
	if (niov < 0)
		return -1;

	/* Check that the data size is not excessive */
	sz = iov_size(&iov[1], niov);
	if (sz > 0xffff)
		return output_binary_error(p, ENOMEM,
			"packet too large, %zu", sz);

	/* Build the three-byte binary header */
	tl[0] = msg;
	tl[1] = (sz >> 8) & 0xff;
	tl[2] = (sz >> 0) & 0xff;
	iov[0].iov_base = tl;
	iov[0].iov_len = 3;

	if (p->on_sendv)
		return p->on_sendv(p, iov, niov + 1);
	return 0;
}

#endif /* !SMALL */
