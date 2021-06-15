/* yc_uring - a yoctochat server using Linux io_uring */

/* The most important thing to understand about io_uring is its actually a
 * facility for asynchronous system calls; that is, it decouples the call from
 * the return. A bit like a future or a promise, if you squint.
 *
 * This is different from earlier event readiness facilities like select(),
 * poll() and epoll. Those tell you that something has happened on a
 * descriptor, and then you can make a blocking, synchronous call.
 *
 * io_uring, on the other hand, gives you set of alternatives for many
 * IO-related syscalls. You give them the same kind of args, and submit them to
 * the kernel via the submission ring. When and if they finish, the results are
 * left on the completion ring. If they don't finish (eg accept() but there's
 * no new connection, readv() but there's nothing waiting to be read) they just
 * sit quietly in the kernel and that's it.
 *
 * So the whole model is around requests and results, not descriptors and
 * readiness. Its interesting!
 *
 * It does make the code more complicated though, because you need to track
 * state so you know what you were doing as you process each completed
 * response.
 *
 * I've spelled a lot the request creation and submission out in full here to
 * avoid the need for jumping around the code while you're trying to follow it!
 *
 * Recommended reading:
 *   https://unixism.net/loti/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <liburing.h>
#include <errno.h>

/* max number of connections. in a real program you probably wouldn't do this,
 * and instead use a more dynamic structure for tracking connections */
#define NUM_CONNS (128)

/* max number of requests in flight. there will be an accept request, one read
 * request per active conn, and potentionally a write per active conn too. so
 * twice NUM_CONNS should be enough. */
#define QUEUE_DEPTH (256)


/* our request objects. we need to make space for request and result data to be
 * stored, as well as things we need to match responses to requests */

/* the kinds of requests will we issue. we store this so we will recognise the
 * responses as they come in */
typedef enum {
  YCR_KIND_ACCEPT,
  YCR_KIND_READ,
  YCR_KIND_WRITE,
  YCR_KIND_CLOSE,
} ycr_kind_t;

/* minimal request; just the kind and the file descriptor it relates to. used
 * as a header for more complex requests */
typedef struct {
  ycr_kind_t ycr_event;
  int        ycr_fd;
} yc_request_t;

/* read/write request. only readv/writev equivalents are available, so we put
 * an iovec in here, and enough buffer space to handle whatever we might read
 * or write. you definitely wouldn't do it this way in a real server */
typedef struct {
  yc_request_t ycr_req;
  struct iovec ycr_iovec;
  char         ycr_iobuf[1024];
} yc_io_request_t;

/* an accept request. carries space for the client IP and port */
typedef struct {
  yc_request_t       ycr_req;
  struct sockaddr_in ycr_addr;
  socklen_t          ycr_addrlen;
} yc_accept_request_t;


/* allocate a minimal request */
static yc_request_t *yc_req_new(ycr_kind_t event, int fd) {
  yc_request_t *req = malloc(sizeof(yc_request_t));
  req->ycr_event = event;
  req->ycr_fd    = fd;
  return req;
}

/* allocate a read/write request for the given fd */
static yc_io_request_t *yc_io_req_new(ycr_kind_t event, int fd) {
  yc_io_request_t *req = malloc(sizeof(yc_io_request_t));
  req->ycr_req.ycr_event  = event;
  req->ycr_req.ycr_fd     = fd;
  req->ycr_iovec.iov_base = req->ycr_iobuf;
  req->ycr_iovec.iov_len  = sizeof(req->ycr_iobuf);
  return req;
}

/* allocate an accept request for the given server fd */
static yc_accept_request_t *yc_accept_req_new(int fd) {
  yc_accept_request_t *req = malloc(sizeof(yc_accept_request_t));
  req->ycr_req.ycr_event = YCR_KIND_ACCEPT;
  req->ycr_req.ycr_fd    = fd;
  req->ycr_addrlen       = sizeof(req->ycr_addr);
  return req;
}

/* free a request; here just for symmetry */
static void yc_req_free(yc_request_t *req) {
  free(req);
}


int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s <port>\n", argv[0]);
    exit(1);
  }

  int port = atoi(argv[1]);
  if (port <= 0) {
    printf("'%s' not a valid port number\n", argv[1]);
    exit(1);
  }

  /* create the server socket */
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    exit(1);
  }

  /* arrange for the listening address to be reusable. This makes TCP
   * marginally "less safe" (for a whole bunch of obscure reasons) but allows
   * us to kill and restart the program with ease */
  int onoff = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &onoff, sizeof(onoff)) < 0) {
    perror("setsockopt");
    exit(1);
  }

  /* set up the address structure for binding, which is *:<port> */
  struct sockaddr_in sin = {
    .sin_family = AF_INET,
    .sin_port   = htons(port),
    .sin_addr   = {
      .s_addr = htonl(INADDR_ANY)
    }
  };
  socklen_t sin_len = sizeof(sin);

  /* bind the server socket to the wanted address */
  if (bind(server_fd, (struct sockaddr *) &sin, sin_len) < 0) {
    perror("bind");
    exit(1);
  }

  /* and open it for connections! */
  if (listen(server_fd, 10) < 0) {
    perror("listen");
    exit(1);
  }

  printf("listening on port %d\n", port);

  struct io_uring ring;
  if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
    perror("io_uring_queue_init");
    exit(1);
  }

  /* create storage for our active connections. in a real server, this would be
   * some mapping from file descriptor -> connection object. here the only
   * thing we're interested is if the descriptor is connected at all, so a bool
   * (int) is enough: if conns[fd] is true, then fd is connected right now */
  int conns[NUM_CONNS];
  memset(&conns, 0, sizeof(conns));

  /* start with async form of accept(). just like the traditional version, it
   * will "block" until there's something to read, but that all happens inside
   * the kernel so we don't have to worry about it.
   *
   * we acquire a free submission queue entry (SQE) from the kernel, set it up
   * for the an async accept(), include our own request state so we can
   * understand the completion queue entry (CQE) that comes back, and submit it
   * for processing */
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  yc_accept_request_t *req = yc_accept_req_new(server_fd);
  io_uring_prep_accept(sqe, server_fd, (struct sockaddr *) &req->ycr_addr, &req->ycr_addrlen, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);

  /* main loop. we just wait until a CQE is available, then process it */
  struct io_uring_cqe *cqe;
  while (io_uring_wait_cqe(&ring, &cqe) >= 0) {

    /* get our own request back. for the moment, just the header */
    yc_request_t *req = (yc_request_t *) cqe->user_data;
    int fd = req->ycr_fd;

    /* the return value of the underlying syscall. typically a negative value
     * will be the negated errno value for the call, so we can still do error
     * handling */
    int res = cqe->res;

    /* do the right thing depending on what kind of request just completed */
    switch (req->ycr_event) {

      /* someone connected! */
      case YCR_KIND_ACCEPT: {
        /* get a handle on the more specialised request */
        yc_accept_request_t *areq = (yc_accept_request_t *) req;

        /* maybe it failed? */
        if (res < 0) {
          /* note negation of return value in place of errno */
          fprintf(stderr, "accept: %s\n", strerror(-res));
        }

        else {
          /* hello! client address is in the req object, because that's what we
           * pointed the request to in io_uring_prep_accept */
          printf("[%d] connect from %s:%d\n", res, inet_ntoa(areq->ycr_addr.sin_addr), ntohs(areq->ycr_addr.sin_port));

          /* remember our new connection. in a real server, you'd create a
           * connection or user object of some sort, maybe send them a
           * greeting, begin authentication, etc */
          conns[res] = 1;

          /* set up an async read for the new connection */
          sqe = io_uring_get_sqe(&ring);
          yc_io_request_t *rreq = yc_io_req_new(YCR_KIND_READ, res);
          io_uring_prep_readv(sqe, res, &rreq->ycr_iovec, 1, 0);
          io_uring_sqe_set_data(sqe, rreq);
          io_uring_submit(&ring);
        }

        /* make a new async accept, since the previous one was consumed. note
         * that we're reusing the request object, but its not special - freeing
         * it and making a new one would also be just fine */
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_accept(sqe, fd, (struct sockaddr *) &areq->ycr_addr, &areq->ycr_addrlen, 0);
        io_uring_sqe_set_data(sqe, areq);
        io_uring_submit(&ring);

        break;
      }

      /* someone sent something */
      case YCR_KIND_READ: {
        /* get a handle on the more specialised request */
        yc_io_request_t *rreq = (yc_io_request_t *) req;

        /* some error, disconnect them */
        if (res < 0) {
          fprintf(stderr, "readv(%d): %s\n", fd, strerror(-res));

          /* free the read request, since we're not going to be reissuing it */
          yc_req_free(req);

          /* make a async close request. we use a minimal request object
           * because close has no interesting args or return; we just need a
           * marker so we can recognise the response for what it is */
          yc_request_t *clreq = yc_req_new(YCR_KIND_CLOSE, fd);
          sqe = io_uring_get_sqe(&ring);
          io_uring_prep_close(sqe, fd);
          io_uring_sqe_set_data(sqe, clreq);
          io_uring_submit(&ring);

          /* mark them "disconnected", so we don't try to send to them while the close request is pending */
          conns[fd] = 0;
        }

        /* zero read, they gracefully closed the connection */
        else if (res == 0) {
          printf("[%d] closed\n", fd);

          /* see error block above, this is the same behaviour */

          yc_req_free(req);

          yc_request_t *clreq = yc_req_new(YCR_KIND_CLOSE, fd);
          sqe = io_uring_get_sqe(&ring);
          io_uring_prep_close(sqe, fd);
          io_uring_sqe_set_data(sqe, clreq);
          io_uring_submit(&ring);

          conns[fd] = 0;
        }

        else {
          /* they sent some data, which is now in the request iobuf (via the
           * iovec we sent in) */
          printf("[%d] read: %.*s\n", fd, res, rreq->ycr_iobuf);

          /* loop over all our connections, and send stuff onto them! */
          for (int dest_fd = 0; dest_fd < NUM_CONNS; dest_fd++) {

            /* take active connections, but not ourselves */
            if (conns[dest_fd] && dest_fd != fd) {

              /* async write request. create new write requests, one for each
               * connection. we copy the data into it but if we were being much
               * cleverer about memory management we could just point the the
               * buffers in the read request, resulting a zero-copy forwarder! */
              yc_io_request_t *wreq = yc_io_req_new(YCR_KIND_WRITE, dest_fd);
              memcpy(wreq->ycr_iobuf, rreq->ycr_iobuf, res);
              wreq->ycr_iovec.iov_len = res;

              sqe = io_uring_get_sqe(&ring);
              io_uring_prep_writev(sqe, dest_fd, &wreq->ycr_iovec, 1, 0);
              io_uring_sqe_set_data(sqe, wreq);
              io_uring_submit(&ring);
            }
          }

        /* make a new async read, since the previous one was consumed. note
         * that we're reusing the request object, but its not special - freeing
         * it and making a new one would also be just fine */
          sqe = io_uring_get_sqe(&ring);
          io_uring_prep_readv(sqe, fd, &rreq->ycr_iovec, 1, 0);
          io_uring_sqe_set_data(sqe, rreq);
          io_uring_submit(&ring);
        }

        break;
      }

      /* they finished receiving what we sent */
      case YCR_KIND_WRITE: {

        /* failed write, so disconnect them */
        if (res < 0) {
          fprintf(stderr, "writev(%d): %s\n", fd, strerror(-res));
          yc_req_free(req);

          /* see read error handling */

          yc_request_t *clreq = yc_req_new(YCR_KIND_CLOSE, fd);
          sqe = io_uring_get_sqe(&ring);
          io_uring_prep_close(sqe, fd);
          io_uring_sqe_set_data(sqe, clreq);
          io_uring_submit(&ring);

          conns[fd] = 0;
        }

        else {
          /* written successfully, so just free the read req */
          yc_req_free(req);
        }

        break;
      }

      /* async close completed */
      case YCR_KIND_CLOSE: {
        /* just free the request, we've already cleaned up and there's nothing
         * useful we could do if the close failed anyway */
        yc_req_free(req);
        break;
      }
    }

    /* mark the CQE "seen", returning it to the ring for reuse */
    io_uring_cqe_seen(&ring, cqe);
  }

  /* io_uring_wait_cqe failed. in a real server you might actually need to
   * handle non-error cases like EINTR, but it complicates this example so we
   * won't bother */
  perror("io_uring_wait_cqe");
  exit(1);
}
