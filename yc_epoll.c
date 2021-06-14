/* yc_epoll - a yoctochat server using a Linux epoll IO loop */

/* NOTE: epoll is simple on the surface, but kinda weird once you get into the
 * high-performance situations where you might actually want to use it.
 * Recommended reading:
 *   https://copyconstruct.medium.com/the-method-to-epolls-madness-d9d2d6378642
 *   https://idea.popcount.org/2017-02-20-epoll-is-fundamentally-broken-12/
 *   https://idea.popcount.org/2017-03-20-epoll-is-fundamentally-broken-22/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <errno.h>

/* max number of connections. in a real program you probably wouldn't do this,
 * and instead use a more dynamic structure for tracking connections */
#define NUM_CONNS (128)

/* max events per call to epoll_wait(). more of them just means fewer calls to
 * epoll_wait() in a busy server, but too many would be a waste of memory. our
 * server is tiny so there's no point having many. */
#define NUM_EVENTS (16)

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

  /* bind the server socket to the wanted address */
  if (bind(server_fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
    perror("bind");
    exit(1);
  }

  /* and open it for connections! */
  if (listen(server_fd, 10) < 0) {
    perror("listen");
    exit(1);
  }

  printf("listening on port %d\n", port);

  /* create the epoll context */
  int epoll = epoll_create1(0);
  if (epoll < 0) {
    perror("epoll_create1");
    exit(1);
  }

  /* create storage for our active connections. in a real server, this would be
   * some mapping from file descriptor -> connection object. here the only
   * thing we're interested is if the descriptor is connected at all, so a bool
   * (int) is enough: if conns[fd] is true, then fd is connected right now */
  int conns[NUM_CONNS];
  memset(&conns, 0, sizeof(conns));

  /* make room for incoming events */
  struct epoll_event events[NUM_EVENTS];

  /* add the server socket; when it becomes "readable", someone connected! note
   * that we use the first element in our events list to set this up just
   * because its convenient; the "event" passed to epoll_ctl() is entirely
   * unrelated to the events returned by epoll_wait() */
  events[0].events = EPOLLIN;
  events[0].data.fd = server_fd;
  if (epoll_ctl(epoll, EPOLL_CTL_ADD, server_fd, &events[0])) {
    perror("epoll_ctl");
    exit(1);
  }

  /* main loop. ask epoll_wait() to tell us if anything interesting happened, or block */
  int nevents;
  while ((nevents = epoll_wait(epoll, events, NUM_EVENTS, -1)) >= 0) {
    /* in theory, nothing could have happened. that should be impossible the
     * way we've set this up but its not an error so might as well quietly
     * handle it */
    if (!nevents)
      continue;

    for (int n = 0; n < nevents; n++) {
      int fd = events[n].data.fd;

      if (fd == server_fd) {
        /* create storage for their address */
        struct sockaddr_in sin;
        socklen_t sinlen = sizeof(sin);

        /* let them in! */
        int new_fd = accept(server_fd, (struct sockaddr *) &sin, &sinlen);
        if (new_fd < 0) {
          perror("accept");
        }
        else {
          /* hello */
          printf("[%d] connect from %s:%d\n", new_fd, inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));

          /* make them non-blocking. this is necessary, because a disconnect will
          * cause a descriptor to become readable, but reading will block
          * forever (because they're disconnected. non-blocking will cause
          * read() to return 0 on a disconnected descriptor, so we can take the
          * right action */
          int onoff = 1;
          if (ioctl(new_fd, FIONBIO, &onoff) < 0) {
            printf("fcntl(%d): %s\n", new_fd, strerror(errno));
            close(new_fd);
            continue;
          }

          /* register the connection with epoll so we can be told when
           * something interesting happens to it. again, its safe to reuse the
           * first element of the events list; even if we're currently
           * processing the first event, we're already done with it */
          events[0].events = EPOLLIN;
          events[0].data.fd = new_fd;
          if (epoll_ctl(epoll, EPOLL_CTL_ADD, new_fd, &events[0]) < 0) {
            printf("epoll_ctl(%d): %s\n", new_fd, strerror(errno));
            close(new_fd);
            continue;
          }

          /* remember our new connection. in a real server, you'd create a
          * connection or user object of some sort, maybe send them a greeting,
          * begin authentication, etc */
          conns[new_fd] = 1;
        }
      }

      else {
        /* yes! */
        printf("[%d] activity\n", fd);

        /* create a buffer to read into */
        char buf[1024];
        int nread = read(fd, buf, sizeof(buf));

        /* see how much we read */
        if (nread < 0) {
          /* less then zero is some error. disconnect them */
          fprintf(stderr, "read(%d): %s\n", fd, strerror(errno));
          epoll_ctl(epoll, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
          conns[fd] = 0;
        }

        else if (nread > 0) {
          /* we got some stuff from them! */
          printf("[%d] read: %.*s\n", fd, nread, buf);

          /* loop over all our connections, and send stuff onto them! */
          for (int dest_fd = 0; dest_fd < NUM_CONNS; dest_fd++) {

            /* take active connections, but not ourselves */
            if (conns[dest_fd] && dest_fd != fd) {

              /* write to them */
              if (write(dest_fd, buf, nread) < 0) {
                /* disconnect if it fails; they might have legitimately gone away without telling us */
                fprintf(stderr, "write(%d): %s\n", dest_fd, strerror(errno));
                epoll_ctl(epoll, EPOLL_CTL_DEL, dest_fd, NULL);
                close(dest_fd);
                conns[dest_fd] = 0;
              }
            }
          }
        }

        /* zero byes read */
        else {
          /* so they gracefully disconnected and we should forget them */
          printf("[%d] closed\n", fd);
          /* must deregister before close, for obscure reasons around epoll's
           * implementation (see notes above) */
          epoll_ctl(epoll, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
          conns[fd] = 0;
        }
      }
    }
  }

  /* epoll_wait failed. in a real server you might actually need to handle
   * non-error cases like EINTR, but it complicates this example so we won't
   * bother */
  perror("epoll_wait");
  exit(1);
}
