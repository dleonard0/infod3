
# infod3 - An introduction

infod3 is a small-footprint key-value store with a subscribe-notify service
for unix systems.

Start your server.

    $ infod &

## Storing and retrieving a value

Let's store the value *Tim* under the key *name*
using the `info` tool.

    $ info name=Tim
    $ info name
    Tim

If you thought that was cool, hold onto your pants.
We're going to do it again using a TCP interface:

    $ nc localhost 26931
    w name Fred
    r name
    INFO "name" "Fred"
    ^C

We typed `r` and `w` commands and the server responded with `INFO`.
The `r` and `w` were aliases for `READ` and `WRITE`.
The server accepted our unquoted string args, because
it's friendly and the strings were simple.
This makes experimentation with the protocol easy,
and, when you need it, robust quoting is available.
You can also type `help` in this channel for syntax help.

The binary AF\_UNIX interface is much more efficient.
It's what the CLI tools and C library use.
See [PROTOCOL](PROTOCOL) for details.

    $ info name
    Fred

## Subscribe-notify

Now we have two terminal sessions, side-by-side.
I've staggered them to make the order of responses clear.
Let's subscribe on the left, and make changes on the right:

    $ nc localhost 26931
    sub foo
                                $ nc localhost 26931
                                w foo bar
    INFO "foo" "bar"
                                w foo snort
    INFO "foo" "snort"
    ^C                          ^C
    $ info -k -s foo
    foo snort
                                $ info -d foo
    foo
                                $ info foo=fighters
    foo fighters
    ^C

The `sub` command subscribed to the key pattern, `foo`,
which matches exactly one key.
We could have subscribed to pattern `*` to see every key,
or to `f*` to see every key starting with 'f'.
You get the idea. It's glob-like.

Subscriptions only exist in their client channel.
They are lost when the connection is closed.

## Transactions

Other clients may change keys at any time.
We want *coherent* keys:

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

The server recorded commands after 'begin' until 'commit'. Then,
all the recorded commands are run together, *atomically*.
The CLI tools use transactions.

## That's it!

You've now seen what infod3 can do:

 - store key-values
 - subscribe-notify
 - transactions
 - text and binary protocols

