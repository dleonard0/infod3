
# infod3 - An introduction

infod3 is a tiny key-value store with a subscribe-notify service
designed for embedded applications.

  * Small client-server protocol
  * Simple server design uses socket buffers and mmap to
    to remain robust under low memory conditions
  * Binary search on lookups
  * Recoverable key-value store file format

## Messages

Client messages:
* `HELLO` _version_ [_text_]  -- request version upgrade.
* `SUB` _pattern_  -- subscribe to async changes to matching keys
* `UNSUB` _pattern_ 
* `READ` _key_  -- request an `INFO` reply for the key
* `WRITE` _key_ [_value_]  -- replace a key's value
* `BEGIN`  -- start batching up messages
* `COMMIT` -- run all batched messages atomically
* `PING` _id_ -- request a matching `PONG`

Server responses:
* `VERSION` _version_ -- response to `HELLO`
* `INFO` _key_ [_value_] -- response to `READ` or `SUB`
* `PONG` _id_ -- response to `PING`
* `ERROR` _id_ _text_ -- error report

See [PROTOCOL](PROTOCOL) for more detail.

## Demo at the command line

Start the server

    $ infod -f /tmp/info.db &

Now [infod(8)](https://github.com/dleonard0/infod3/wiki/infod%288%29)
is listening on unix socket `@INFOD` (and on TCP port 26931 if that
was compiled in).

Let's store the value *Tim* under the key *name* in the new data store.

    $ info name=Tim
    $ info name
    Tim

The [info(1)](https://github.com/dleonard0/infod3/wiki/info%281%29)
tool is mainly a basic query/update tool.

In another window, let's try subscribe to ongoing changes to key *name*:

    $ info -s name
    Tim
    ...

The first time the subscription is registered, the client displays all
the current values.

To see all the key-values you could subscribe to `*`, enable the option
to display keys with values, and set the timeout option to quit immediately
the subscription starts, but as a convenience you can just use `info -A`:

    $ info -A
    name=Tim

## Demo from C

A C library
[libinfo(3)](https://github.com/dleonard0/infod3/wiki/libinfo%283%29)
is provided to accommodate simple clients.
It needs no initialisation and has no dependencies.

Simple producers:
```c
#include <info.h>

int
main()
{
	int r;

	/* Publish a string value */
	r = info_writes("name", "Tim");
	if (r == -1)
		perror("info_writes");
}
```

Simple consumers:
```c
{
	char buf[1024];
	int len;

	/* Read a string value */
	len = info_reads("name", buf, sizeof buf);
    	if (len != -1)
		printf("got name=%s\n", buf);
	else if (errno == ENOENT)
		printf("name was deleted\n");
	else
		perror("info_reads");
}
```

Ongoing consumers provide a callback function:
```c
static int
callback(const char *key, const char *value, unsigned int valuelen)
{
	printf("got %s=%.*s\n", key, valuelen, value);
	return 1; /* Keep waiting. */
}

int
main()
{
	info_tx_begin();
	info_tx_sub("f*");
	info_tx_commit(callback); /* Receives first update of f* keys */
	info_loop(callback);  /* Ongoing receipt of updates */
}
```

For sophisticated clients you may be better off reading the
[PROTOCOL](PROTOCOL)
specification and writing messages directly to the server socket.


## Protocol demo using TCP

You can explore the protocol using a TCP client like `nc`.
Let's send some READ and WRITE command messages. Type the following:

    $ nc localhost 26931
    WRITE "name" "Fred"
    READ "name"

The server will respond to the READ with an INFO response:

    INFO "name" "Fred"

Keys are deleted by omitting the value part.

    WRITE "name"

You can also send `HELP` for syntax help.
See [PROTOCOL](PROTOCOL) for details.
For tiny systems, the TCP service, help and verbosity
can be dropped by compiling with `-DSMALL`.

## TCP subscribe-notify

The subscription command SUB is effectively an ongoing READ.
After SUB is sent, INFO reposnses are received asynchronously.

When a WRITE occurs in one client channel, the server checks
every channel with an active SUB pattern to see if it should be
informed of the update.

Here we show two terminal client sessions, side-by-side,
with blank lines to make the order of responses clear.
We send a SUB on the left and a WRITE on the right.
The server responds with an INFO.

    $ nc localhost 26931      | $ nc localhost 26931
    SUB "foo"                 |
                              | WRITE "foo" "bar"
    INFO "foo" "bar"          |

SUB suppresses duplicates, but deletes remain visible.

                              | WRITE "foo" "bar"
                              | WRITE "foo"
    INFO "foo"                |
                              | WRITE "foo" "snort"
    INFO "foo" "snort"        |

The `SUB` command takes a wildcard pattern, here `foo`.
We could have subscribed to pattern `*` to see every key,
or to `f*` to see every key starting with 'f'.
You get the idea.
It's glob-like with metachars `*` `?` and `\`.

Subscriptions only exist in the client channel.
They are lost when the connection is closed.

## Demo of subscribe-notify from command line

The CLI tool can subscribe with the `-s` option.
In this demo we add `-k=` so that the received key is also displayed:

    $ info -k= -s foo    |
    foo=snort            |
                         |  $ info -d foo
    foo                  |
                         |  $ info foo=fighters
    foo=fighters         |

## Transactions

Clients may change multiple keys in such a way that updates
are seen "atomically"  by other clients.
Batched commands are bracketed between `BEGIN` and `COMMIT`.

    $ nc localhost 26931
    BEGIN
    READ "dog.name"
    READ "dog.length"
    READ "dog.toy"
    PING
    COMMIT

Then the server will reply:

    INFO "dog.name" "Fred"
    INFO "dog.length" "0.7 m"
    INFO "dog.toy"
    PONG

Batched commands are "played back" without interleaving commands
from any other client.
The CLI tool always uses transactions.
## That's it!

You've now seen what infod3 can do:

 - store key-values
 - subscribe-notify
 - transactions
 - text and binary protocols

