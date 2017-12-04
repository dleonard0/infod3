#pragma once

#define CMD_HELLO		0x00	/* %c<id> [%s<text>] */
#define CMD_SUB			0x01	/* %s<pattern> */
#define CMD_UNSUB		0x02	/* %s<pattern> */
#define CMD_GET			0x03	/* %s<key> */
#define CMD_PUT			0x04	/* %s<key>
					 | %s<key>%c<0>%*s<value>
					 | %*s<key\0val> */
#define CMD_BEGIN		0x05
#define CMD_COMMIT		0x06
#define CMD_PING		0x07	/* %s<id> */

#define MSG_VERSION		0x80	/* %c<id> [%s<text>] */
#define MSG_INFO		0x81	/* %s<key>
					 | %s<key>%c<0>%*s<value>
					 | %*s<key\0val> */
#define MSG_PONG		0x82	/* "[%s]", [id] */
#define MSG_ERROR		0x83	/* "%s" */

#define MSG_EOF			0xff	/* pseudo-message indicating close */

/*
 * A protocol:
 *                     ___
 *         <- on_send |   | <-- output
 *  network           |   |
 *         -> recv    |___| --> on_input
 *
 * The proto will select its mode on the first send or recv from the network.
 * Outputs and inputs are always in the form of <cmd,data>.
 */

struct proto;
struct iovec;

struct proto *proto_new(void);
void proto_free(struct proto *p);

void proto_set_udata(struct proto *p, void *udata, void (*ufree)(void *));
void *proto_get_udata(struct proto *p);

int proto_recv(struct proto *p, const void *net, unsigned int netlen);
void proto_set_on_input(struct proto *p,
	int (*on_input)(struct proto *p, unsigned char msg,
			 const char *data, unsigned int datalen));

/* proto_output message format:
 *  %c   - byte (unsigned)
 *  %s   - NUL-terminated string (but no NUL is emitted)
 *  %*s  - size_t, binary data
 * Space chars are ignored.
 * No other formats or literal characters are permitted.
 * The first argument must be a %c with the message/command ID.
 * Returns 0 on success, or -1 on error.  */
__attribute__((format(printf, 2, 3)))
int proto_output(struct proto *p, const char *fmt, ...);
void proto_set_on_sendv(struct proto *p,
    int (*on_sendv)(struct proto *p, const struct iovec *, int));

/* The default error handler is to print the message to stderr */
void proto_set_on_error(struct proto *p,
    void (*on_error)(struct proto *p, const char *msg));
