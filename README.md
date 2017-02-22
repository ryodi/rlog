rlog
====

A small ring-buffer logger for keeping diagnostic logging under
control (specifically in space-constrained Docker containers).

Installation
------------

To build the software, just run `make`:

    make

This will result in a binary, named `rlog` in the current working
directory.

If you are on Linux, and want a static binary, try `make static`
instead; the resulting binary will be called `rlog-static`, and
not link in any libraries -- perfect for using in a Docker image!

Usage
-----

`rlog` reads every line on standard input, datestamp it,
shove it in a ring buffer (possibly shoving out older messages in
the process) and send it to connected clients.  To pull off that
last bit, it binds a TCP socket, usually localhost:1040, to await
client connections.

Usage:

    rlog [127.0.0.1:1040]

When a client connects, they will immediately be sent the entire
contents of the ring buffer, to "catch up".  `nc` makes a great
client to `rlog`, and it's fairly ubiquitous.

Once stdin is closed, and all messages have been sent to all
connected clients, `rlog` will shut down.

Contributing
------------

Fork it and submit a pull request.

About
-----

But what about syslog?  Look, I love logs as much as the next
sysad, but they tend to get in the way.  Always sitting there
taking up disk space.  I usually want debug logs while I'm
troubleshooting an uncooperative server, and I rarely look at them
otherwise.  Nothing like this existed in the wild, so I wrote it.
