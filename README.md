# yoctochat

## The tiniest chat servers on earth!

Here will be a collection of the simplest possible TCP chat servers, to demonstrate how to write multiuser, multiplexing server software using various techniques.

### The "spec"

A `yoctochat` server will:

* take a single commandline argument, the port to listen on
* open a listening port
* handle multiple connections and disconnections on that port
* receive text on a connection, and forward it on to all other connections
* produce simple output about what its doing
* demonstrate a single IO multiplexing technique as simply as possible
* be well commented!

### Why?

20+ years ago, during my University days, I started writing little chat servers like this to teach myself C, UNIX, systems programming, internet programming, and so on. I went on to write bigger and better ones. It has been useful knowledge!

Lately, I found myself wanting to experiment with [io_uring](https://unixism.net/loti/), and I realised I'd forgotten how the "classic" `select()` loop is constructed. So I started there, and here we are!
