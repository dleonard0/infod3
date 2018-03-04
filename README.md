
# infod3 - An introduction

infod3 is a small-footprint key-value store with a subscribe-notify service
for unix systems.

  * Bare bones client-server protocol over self-framed unix sockets
  * Optionally-built TCP variant useful for command line diag with nc
  * Small server is single-threaded, exploits mmap and socket buffers
    to be robust under low memory conditions

## Demo: Taking infod3 for a spin

Start your server.

    $ infod &

## Using the CLI tool

Let's store the value *Tim* under the key *name*
using the `info` CLI tool.

    $ info name=Tim
    $ info name
    Tim

The CLI tool and the libinfo C library use the efficient
binary AF\_UNIX interface.

## Using the TCP socket

Beginners can explore the service using a TCP interface and
a tool like `nc`.
Let's send some READ and WRITE command messages:

    $ nc localhost 26931
    WRITE "name" "Fred"
    READ "name"

and the server responds with an INFO notification:

    INFO "name" "Fred"

Keys are deleted by omitting the value part.
You can also type `HELP` in this channel for syntax help.
See [PROTOCOL](PROTOCOL) for details.

## Subscribe-notify

Here we show two terminal sessions, side-by-side.
I've staggered them to make the order of responses clear.
We will "subscribe" to the key *foo* on the left,
and write changes to the key on the right:

    $ nc localhost 26931
    SUB "foo"
                                $ nc localhost 26931
                                WRITE "foo" "bar"
    INFO "foo" "bar"

On the left, an immediate INFO update appeared to follow the WRITE.

                                WRITE "foo" "snort"
    INFO "foo" "snort"

The CLI tool can also subscribe using `-s`.
We add `-k` so that the received key is also displayed:

    $ info -k -s foo
    foo snort
                                $ info -d foo
    foo
                                $ info foo=fighters
    foo fighters

The `sub` command takes a key pattern, here `foo`,
which matches exactly one key.
We could have subscribed to pattern `*` to see every key,
or to `f*` to see every key starting with 'f'.
You get the idea. It's glob-like.

Subscriptions only exist in the client channel.
They are lost when the connection is closed.

## Transactions

Clients may change multiple keys in such a way that updates
are seen coherently by other clients.

    $ nc localhost 26931
    begin
    read dog.name
    read dog.length
    read dog.toy
    ping
    commit
    INFO "dog.name" "Fred"
    INFO "dog.length" "0.7 m"
    INFO "dog.toy"
    PONG
    ^C

The server records the commands between 'begin' and 'commit'. Then,
plays them all back by excluding any other client.
The CLI tool always uses transactions.

## From C

The C library is provided for simple use cases.
It auto-initialises and makes being a producer client very simple.

    #include <info.h>

    int
    main() {
        info_writes("name", "Tim");
    }

Errors (not shown here) return -1 and set `errno`.

To be a one-shot consumer, you provide the value buffer:

    {
    	char buf[1024];
	int len;

        len = info_read("name", buf, sizeof buf);
	if (len > 0)
		printf("I got name=%.*s\n", len, buf);
    }

To be an ongoing consumer, start a subscription in a transaction.

    int
    callback(const char *key, const char *value, unsigned int sz)
    {
    	printf("I got %s=%.*s\n", key, sz, value);
	return 1; /* Keep waiting */
    }

    int
    main() {
    	info_tx_begin();
	info_tx_sub("f*");
	info_tx_commit(callback); /* Receives all existing f* keys */
	info_sub_wait(callback);  /* Receives updates f* keys */
    }

The libinfo.a library is intended for simple C clients.
For sophisticated clients you may be better off reading the PROTOCOL
specification and writing messages directly to the server socket.

## That's it!

You've now seen what infod3 can do:

 - store key-values
 - subscribe-notify
 - transactions
 - text and binary protocols

