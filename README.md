
# infod3 - An introduction

infod3 is a tiny key-value store with a subscribe-notify service
for embedded applications.

  * Lightweight client-server protocol using self-framed unix sockets
  * Optional TCP listener for protocol diag with nc
  * Server process is single-threaded, exploits mmap and socket buffers
    to remain robust under low memory conditions

## Start the server

    $ infod -f /tmp/info.db &

It listens on unix socket `@INFOD` and on TCP port 26931.

## Using the CLI tool

Let's store the value *Tim* under the key *name*
using the `info` CLI tool.

    $ info name=Tim
    $ info name
    Tim

In another window, you can subscribe to ongoing changes in *name*:

    $ info -s name
    Tim
    ...

Dump all the keys:

    $ info -A
    name=Tim

See the manual page info(1) for details.

## Using the TCP socket

You can explore the protocol using a TCP client like `nc`.
Let's send some READ and WRITE command messages:

    $ nc localhost 26931
    WRITE "name" "Fred"
    READ "name"

The server will respond to READ with an INFO notification:

    INFO "name" "Fred"

Keys are deleted by omitting the value part.

    WRITE "name"

You can also send `HELP` in this channel for syntax help.
See [PROTOCOL](PROTOCOL) for details.
For tiny systems, the TCP service, help and verbosity
can be dropped by compiling with `-DSMALL`.

## TCP subscribe-notify

Subscription command SUB is an ongoing READ.

Here we show two terminal sessions, side-by-side,
with blank lines to make the order of responses clear.
We send a SUB on the left and a WRITE on the right:

    $ nc localhost 26931      | $ nc localhost 26931
    SUB "foo"                 |
                              | WRITE "foo" "bar"
    INFO "foo" "bar"          |

Duplicates are suppressed. Deletes are visible.

                              | WRITE "foo" "bar"
                              | WRITE "foo"
    INFO "foo"                |
                              | WRITE "foo" "snort"
    INFO "foo" "snort"        |

The `SUB` command takes a key pattern, here `foo`,
which matches exactly one key.
We could have subscribed to pattern `*` to see every key,
or to `f*` to see every key starting with 'f'.
You get the idea.
It's glob-like with metachars `*` `?` and `\`.

Subscriptions only exist in the client channel.
They are lost when the connection is closed.

## CLI subscribe-notify

The CLI tool subscribes using `-s`.
We add `-k=` so that the received key is also displayed:

    $ info -k= -s foo
    foo=snort
                                $ info -d foo
    foo
                                $ info foo=fighters
    foo=fighters

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

## From C

A C library is provided to accommodate simple clients.
It needs no initialisation or dependencies.

    #include <info.h>

    int
    main() {
        int r;
        
        r = info_writes("name", "Tim");
        if (r == -1)
                perror("info_writes");
    }

Consumers provide a buffer:

    {
        char buf[1024];
        int len;
        
        len = info_read("name", buf, sizeof buf);
        if (len == -1)
                perror("info_reads");
        else
                printf("got name=%.*s\n", len, buf);
    }

Subscriptions require a callback function:

    int
    callback(const char *key, const char *value, unsigned int valuelen)
    {
        printf("got %s=%.*s\n", key, valuelen, value);
        return 1; /* Keep waiting. */
    }

    int
    main() {
        info_tx_begin();
        info_tx_sub("f*");
        info_tx_commit(callback); /* Receives first update of f* keys */
        info_sub_wait(callback);  /* Ongoing receipt of f* updates */
    }

The libinfo library is re-entrant but not thread safe.
For sophisticated clients you may be better off reading the
[PROTOCOL](PROTOCOL)
specification and writing messages directly to the server socket.

## That's it!

You've now seen what infod3 can do:

 - store key-values
 - subscribe-notify
 - transactions
 - text and binary protocols

