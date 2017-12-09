#include <errno.h>
#include <sys/uio.h>

#include "proto.h"
#include "protopriv.h"

/*
 * Raises a local error produced from the proto_output() path in binary mode.
 * Prints an error message on stderr.
 * Sets errno.
 * Returns -1.
 */
int
output_binary_error(struct proto *p, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	proto_errorv(p, err, "proto_output() binary", fmt, ap);
	va_end(ap);
	return -1;
}

/* Convert the format string into an iov list.
 * The format string only supports %c %s and %*s, and spaces are ignored.
 * Approximately one iov per format is used.
 * The work buffer is used to collect %c output, and returned iovs
 * may point into it.
 * Returns number of iov created, or -1 on error. */
int
to_binary_iov(struct proto *p, struct iovec *iov, int maxiov,
	char *work, size_t worksz,
	const char *fmt, va_list ap)
{
	struct iovec *iov_start = iov;
	char *str;
	char f;
	int worklen;

	worklen = 0;
	while ((f = *fmt++)) {
		if (f == ' ')
			continue;
		if (f != '%')
			return output_binary_error(p, EINVAL,
				"unexpected format char %c", f);
		if (iov >= iov_start + maxiov) {
			errno = ENOMEM;
			return -1;	/* too many % args */
		}
		switch (*fmt++) {
		case '*':
			if (*fmt++ != 's') abort();
			iov->iov_len = va_arg(ap, int);
			iov->iov_base = va_arg(ap, char *);
			iov++;
			break;
		case 's':
			str = va_arg(ap, char *);
			iov->iov_len = strlen(str);
			iov->iov_base = (void *)str;
			iov++;
			break;
		case 'c':
			if (worklen >= worksz) {
				errno = ENOMEM;
				return -1; /* too many %c */
			}
			work[worklen] = va_arg(ap, int) & 0xff; /* promoted */
			iov->iov_base = &work[worklen];
			iov->iov_len = 1;
			worklen++;
			iov++;
			break;
		default:
			return output_binary_error(p, EINVAL,
				"unknown format %%%c", *(fmt - 1));
		}
	}
	return iov - iov_start;
}

int
recv_framed(struct proto *p, const char *net, unsigned int netlen)
{
	if (!p->on_input)
		return netlen;
	if (!netlen)
		return p->on_input(p, MSG_EOF, NULL, 0);
	return p->on_input(p, *net, net + 1, netlen - 1);
}

int
output_framed(struct proto *p, unsigned char msg, const char *fmt, va_list ap)
{
	struct iovec iov[8];
	char work[8];
	int niov = to_binary_iov(p, &iov[1], ARRAY_SIZE(iov) - 1,
		work, sizeof work, fmt, ap);
	if (niov < 0)
		return -1;
	iov[0].iov_base = &msg;
	iov[0].iov_len = 1;
	if (p->on_sendv)
		return p->on_sendv(p, iov, niov + 1);
	return 0;
}

