#pragma once
#include <stdarg.h>

/*
 * infod3 network protocol translator.
 *
 * SUMMARY
 *
 * The protocol translator (proto) provides two callable interfaces
 * (proto_recv(), proto_output()) and accepts two event callback
 * function pointers (on_input, on_send):
 *                         _______
 *          proto_recv => |       | => on_input
 *  network               | proto |                  application
 *             on_send <= |_______| <= proto_output
 *
 * The proto auto-selects the network mode (binary or text) from the first
 * on_send or proto_recv on the network. If on_send is first, it will prefer
 * to talk binary. The selected mode is invisible to the application.
 *
 * An application's on_input() callback always recieves a PDU as a <cmd,data[]>
 * pair. The application sends PDUs through proto_output() which uses a
 * printf-like signature: the first %c format provides the cmd ID, and
 * following %-formats encode the data[].
 *
 * On the network side, the interfaces proto_recv() and on_send() use
 * char[] and iovec[], intended to match up with read() and writev().
 *
 * The protocol translator generally DOES NOT interpret any messages
 * it translates. It may interpret the protocol version in a HELLO message
 * in a future version. Some malformed received network messages will
 * result in a network protocol error reply.
 *
 *
 * Input/from-net function call chart:
 *
 * |                               _____                                  __
 * `-> char[] frag  ->.           |     | ->  MSG_*, char[]  ->.         |A |
 *                     proto_recv()     |                       on_input()  |
 *  <-  { 1+ ok     <-'           |     | <-  { 1+ ok        <-'         |__|
 *      { 0  ok,close             |_____|     { 0  ok,close
 *      {-1  error,close            \         {-1  error,close
 *                                   v
 *                               MSG_ERROR (protocol error)
 * Output/to-net chart:              |
 *        __                       _/___                                      |
 *       |N |       .<-  iov[] <- |     |            .<- "%c %*s",MSG_*,... <-'
 *       |  on_sendv()            |     proto_output()
 *       |__|       `-> 0+ ok  -> |     |            `-> 0+ ok              ->
 *                     -1  error  |_____|               -1  error
 *
 * Return code convention:
 *     -1  for unrecoverable error; errno must be set
 *      0  for successful completion
 *      1+ for ongoing or partial success (return value > 0)
 *
 */
struct proto;

/*
 * Message and command IDs.
 * Client applications send CMDs, and server applications send MSGs.
 * (These could have been called 'request' and 'reply', but they weren't.)
 */

#define CMD_HELLO		0x00	/* %c[%s], <id>[,<text>] */
#define CMD_SUB			0x01	/* %s, <pattern> */
#define CMD_UNSUB		0x02	/* %s, <pattern> */
#define CMD_READ		0x03	/* %s, <key> */
#define CMD_WRITE		0x04	/* %s, <key>
					 | %s%c%*s <key>,0,<value>
					 | %*s     <key\0val> */
#define CMD_BEGIN		0x05
#define CMD_COMMIT		0x06
#define CMD_PING		0x07	/* %s, <id> */

#define MSG_VERSION		0x80	/* %c[%s], <id>[,<text>] */
#define MSG_INFO		0x81	/* %s, <key>
					 | %s%c%*s, <key>,0,<value>
					 | %*s, <key\0val> */
#define MSG_PONG		0x82	/* [%s], [id] */
#define MSG_ERROR		0x83	/* %s, <humantext> */

#define MSG_EOF			0xff	/* pseudo-message indicating close
                                         * i.e. proto_recv(netlen=0) */

struct proto *proto_new(void);
void proto_free(struct proto *p);

/* Stores an application pointer (udata) in the protocol.
 * If the proto is freed, or if the udata is set again later,
 * the optional ufree function will be called on the old udata. */
void proto_set_udata(struct proto *p, void *udata, void (*ufree)(void *));
void *proto_get_udata(struct proto *p);

/* proto_recv():
 *  Receive a PDU or partial PDU from the network.
 *  Parameters net,netlen describe the binary data received.
 *  Pass a netlen of 0 to indicate the peer closed the connection.
 *  The net parameter (if not NULL) must point to a buffer of
 *  at least netlen+1 bytes in length, and guarantee that
 *  the byte at net+netlen is NUL.
 *  Returns -1 on unrecoverable protocol error (eg ENOMEM).
 *  Returns 0 or -1 to indicate the connection should be closed.
 */
int proto_recv(struct proto *p, const void *net, unsigned int netlen);

#define PROTO_RECVSZ 65536

/* Sets the callback for received decoded messages.
 *
 * on_input():
 *  The <msg,data> parameters hold the received message from the network.
 *  MSG_EOF indicates the peer closed the connection, and on_input
 *  should return 0 in that case.
 *  The data parameter points to a buffer that is at least datalen+1
 *  bytes long, and guarantees that data[datalen]=='\0'.
 *
 *  The value returned by on_input() generally becomes that returned
 *  returned by proto_recv().
 *  Return 0 or -1 to request an immediate protocol shutdown and to drop
 *  any partially received messages.
 *  Return -1 to indicate an abnormal errors you indicate by errno (eg ENOMEM).
 *  Return 1 to indicate the message was serviced and you can service more.
 *  It is permissible for on_input() to call proto_output().
 */
void proto_set_on_input(struct proto *p,
	int (*on_input)(struct proto *p, unsigned char msg,
			 const char *data, unsigned int datalen));

/* proto_output():
 *  Encode and send a PDU.
 *  The fmt argument is similat to printf but only understands the
 *  following formats:
 *     %c    - byte (unsigned)
 *     %s    - NUL-terminated string (but no NUL is emitted)
 *     %*s   - size_t, binary data
 *     " "   - space characters (\040) are ignored
 *  No other formats or literal characters are permitted, and
 *  will result in EINVAL.
 *  Returns 0+ on success.
 *  Returns -1 on error (EINVAL, ENOMEM).
 */
_Pragma("GCC diagnostic ignored \"-Wformat-zero-length\"")
__attribute__((format(printf, 3, 4)))
int proto_output(struct proto *p, unsigned char msg, const char *fmt, ...);
int proto_outputv(struct proto *p, unsigned char msg, const char *fmt,
	va_list ap);

/* Sends a MSG_ERROR to the peer, fmt is human text */
__attribute__((format(printf, 3, 4)))
int proto_output_error(struct proto *p, unsigned char code,
	const char *fmt, ...);

#define PROTO_ERROR_BAD_MSG		100
#define PROTO_ERROR_BAD_ARG		101
#define PROTO_ERROR_TOO_BIG		102
#define PROTO_ERROR_BAD_SEQ		103
#define PROTO_ERROR_INTERNAL		255

struct iovec;

/* Sets the upcall for delivering a PDU to the network.
 *
 * on_sendv():
 *  The iovs[] array contains a single PDU for transmission to
 *  the network. It will never be empty.
 *
 *  Return -1 if there was an error sending the PDU, and set errno.
 *  Return 0+ to indicate success.
 *  Generally, the return value from on_sendv() becomes the returned
 *  value from proto_output().
 */
void proto_set_on_sendv(struct proto *p,
    int (*on_sendv)(struct proto *p, const struct iovec *iovs, int niovs));

/* Sets an upcall handler to receive error messages generated
 * within the protocol (This is NOT from server MSG_ERROR).
 * If NULL or unset, messages will be sent to stderr. */
void proto_set_on_error(struct proto *p,
    void (*on_error)(struct proto *p, const char *msg));

#define PROTO_MODE_UNKNOWN	0	/* still auto-detecting */
#define PROTO_MODE_BINARY	1	/* TLV */
#define PROTO_MODE_TEXT		2	/* telnet ascii */
#define PROTO_MODE_FRAMED	3	/* TV */
int proto_set_mode(struct proto *p, int mode);
int proto_get_mode(struct proto *p);

/*
 * Note: PROTO_MODE_FRAMED is an efficient mode that can be used
 * when the transport stream provides its own framing. (For example
 * Linux's SOCK_SEQPACKET). If this mode is used, proto_recv() must
 * be used to send exactly one PDU at a time. In return, on_send()
 * will similarly be called with exactly one PDU at a time.
 * This mode operates with very few buffer copies.
 */
