
# infod3 - An introduction

infod3 is a small-footprint key-value store with subscribe-notify service
for unix systems.

Start your server.

    $ infod &

## Storing and retrieving a value

    $ info -w name=Tim
    $ info -r name
    Tim

We just stored the value *Tim* with the key *name*
using the CLI tool, `info`.

If you thought that was cool, hold onto your pants.
We're going to do it again using a TCP interface:

    $ nc localhost 26931
    w name Fred
    r name
    INFO "name" "Fred"
    ^C

We typed `r` and `w` commands and the server responded with `INFO`.
Case in this channel doesn't matter actually.
The `r` and `w` were also aliases for `read` and `write`.
And the server accepted our unquoted strings, because they were simple.
This makes command-line experimentation with the protocol easy.
You can type `help` in that channel too.

The binary interface is much more efficient.
It's what the CLI tools and library use.
See [PROTOCOL](PROTOCOL) for details.
There's a C library for clients of course.

    $ info -r name
    Fred

## Subscribe-notify

We have two terminal sessions.
I've inserted blank lines to make the order of responses clear.

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
                                $ info -w foo=fighters
    foo fighters
    ^C

The 'sub' command *subscribes* to a key pattern `foo`.
That pattern matches exactly one key, foo.
I could have subscribed to pattern `*` to see every key,
or `f*` to see every key starting with 'f'.
The pattern language is glob-like but simpler (see docs).

On the right-hand side I started changing the value of key foo,
and you can see that the left-hand session was immediately notified of
those changes.
The bottom half of the example shows the same thing but using
the 'info' tool.

Subscriptions only exist in the client channel.
They are lost when the connection is closed.

## Transactions

    $ nc localhost 26931
    begin
    read name
    read foo
    read quz
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
'read' commands.

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
