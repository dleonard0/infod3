
# infod3 - An introduction

infod3 is a small-footprint key-value store with subscribe-notify service
for unix systems.

Start your server.

    $ infod &

## Storing and retrieving a value

    $ info -w name=Tim
    $ info name
    Tim

We just stored the value *Tim* with the key *name*
using the CLI tool, `info`.

If you thought that was cool, hold onto your pants.
We're going to do it again using a TCP interface:

    $ nc localhost 26931
    put name Fred
    get name
    INFO "name" "Fred"
    ^C
    
    $ info name
    Fred

We typed `put` and `get` in lowercase, and the server's
reponses came back in uppercase `INFO`, but actually case doesn't matter.
And the server accepted our unquoted strings, because they were simple.
That makes experimentation easy.

There is also a binary interface which is much more efficient.
It's what the CLI tools and library actually use.
See [PROTOCOL](PROTOCOL) for details.

## Subscribe-notify

    $ nc localhost 26931
    sub foo
                                $ nc localhost 26931
                                put foo bar
    INFO "foo" "bar"
                                put foo snort
    INFO "foo" "snort"
    ^C                          ^C
    $ info -s foo
    foo snort
                                $ info -d foo
    foo
                                $ info -w foo=fighters
    foo fighters
    ^C

The first 'sub' command subscribed to a key pattern *foo* that matches
just one key, foo. I could have subscribed to pattern "\*" to see every
key. The pattern language is glob-like but simpler. Subscriptions are
canceled when the connection is closed.

On the right-hand side I started changing the value of key foo,
and you can see that the left-hand session was immediately notified of
those changes.
The bottom half of the example shows the same thing but using
the 'info' tool.

## Transactions

    $ nc localhost 26931
    begin
    get name
    get foo
    get quz
    ping
    commit
    INFO "name" "Fred"
    INFO "foo" "fighters"
    INFO "quz"
    PONG
    ^C

The server recorded commands after 'begin' until 'commit'. Then,
all the recorded commands are run at once, atomically.
This is for when you want a *coherent* view of multiple
values, without having to worry that values changed between your
'get' commands.

The CLI tools uses transactions.

## That's it!

You've now seen the entire feature set of infod3:

 - key-value store
 - subscribe-notify
 - transactions
 - text and binary equivalent protocols

## Using the C library

```c
tbd

```
