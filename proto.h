#pragma once

/*
 * infod3 network protocol translator.
 *                     ___
 *         <- on_send |   | <-- output
 *  network           |   |               application
 *         -> recv    |___| --> on_input
 *
 * The protocol will auto-select its mode (binary or text) from the first
 * send or recv from the network. If it sends first, it will prefer binary.
 *
 * Application input is always in the pair form of <cmd,data>.
 * Application output uses a printf-like interface where the first %c
 * format corresponds to the message ID.
 */

/* Message IDs. Clients send CMDs, servers send MSGs.
 * The protocol object can send and receive either. */

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

struct proto;
struct iovec;

struct proto *proto_new(void);
void proto_free(struct proto *p);

/* Stores an application pointer (udata) in the protocol.
 * If the proto is freed, or the udata is altered,
 * the optional ufree function will be called on the current udata. */
void proto_set_udata(struct proto *p, void *udata, void (*ufree)(void *));
void *proto_get_udata(struct proto *p);

/* Passs network-received pdu to the protocol.
 * A partial PDU may be received, except in PROTO_MODE_FRAMED, where
 * the PDUs must be recv'd completely and independently.
 * Returns -1 on an unrecoverable error (eg ENOMEM). */
int proto_recv(struct proto *p, const void *net, unsigned int netlen);
/* Sets the callback for received decoded messages.
 * At close, the callback will receive MSG_EOF.
 * On unrecoverable errors, the callback function should set errno
 * and return -1.
 * To discard buffered data and return 0 from proto_recv, the callback
 * should return 0. This is how the callback requests an orderly close.
 */
void proto_set_on_input(struct proto *p,
	int (*on_input)(struct proto *p, unsigned char msg,
			 const char *data, unsigned int datalen));

/* Encode and send a protocol message.
 * The format string only understands the following:
 *  %c   - byte (unsigned)
 *  %s   - NUL-terminated string (but no NUL is emitted)
 *  %*s  - size_t, binary data
 *  space chars are ignored
 * No other formats or literal characters are permitted.
 * The first format must be %c with the message/command ID.
 * Returns 0 on success, or -1 on error (EINVAL, ENOMEM). */
__attribute__((format(printf, 2, 3)))
int proto_output(struct proto *p, const char *fmt, ...);
/* Sets the upcall for delivering a PDU to the network.
 * In PROTO_MODE_FRAMED, the iovs contain a single PDU,
 * whose framing must be preserved by the upcall.
 * The iovs passed to the upcall will never be empty.
 * The upcall should return 0 or >0 on success.
 * The upcall must return -1 if it was unable to transmit or
 * to buffer the entire PDU, and set errno accordingly. */
void proto_set_on_sendv(struct proto *p,
    int (*on_sendv)(struct proto *p, const struct iovec *iovs, int niovs));

/* Sets an upcall handler to receive error messages generated
 * within the protocl.
 * If NULL or unset, messages will be sent to stderr. */
void proto_set_on_error(struct proto *p,
    void (*on_error)(struct proto *p, const char *msg));

#define PROTO_MODE_UNKNOWN	0	/* still auto-detecting */
#define PROTO_MODE_BINARY	1	/* TLV */
#define PROTO_MODE_TEXT		2	/* telnet ascii */
#define PROTO_MODE_FRAMED	3	/* TV */
int proto_set_mode(struct proto *p, int mode);
int proto_get_mode(struct proto *p);
