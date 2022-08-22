/* yc_select - a yoctochat server using a classic select() IO loop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <errno.h>

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

  /* create storage for our active connections. in a real server, this would be
   * some mapping from file descriptor -> connection object. here the only
   * thing we're interested is if the descriptor is connected at all, so a bool
   * (int) is enough: if conns[fd] is true, then fd is connected right now */
  int conns[FD_SETSIZE];
  memset(&conns, 0, sizeof(conns));

  /* create the fd_set we will use to register interest in read events */
  fd_set rfds;
  FD_ZERO(&rfds);

  /* add the server socket; when it becomes "readable", someone connected! */
  FD_SET(server_fd, &rfds);

  /* we need to tell select() what the upper descriptor in the set is, so it
   * knows when to stop scanning. honestly these days we could just use
   * FD_SETSIZE because its laughably small (1024), but this is history */
  int max_fd = server_fd+1;

  /* the main IO loop! call select, ask it to check the descriptors we're
   * interested in. any descriptors in the set that aren't have no new activity
   * will be cleared; any remaining set have activity on them */
  while (select(max_fd, &rfds, NULL, NULL, NULL) >= 0) {

    /* if the server socket has activity, someone connected */
    if (FD_ISSET(server_fd, &rfds)) {
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

        /* remember our new connection. in a real server, you'd create a
         * connection or user object of some sort, maybe send them a greeting,
         * begin authentication, etc */
        conns[new_fd] = 1;
      }
    }

    /* loop over all our connections, seeing if anything happend */
    for (int fd = 0; fd < FD_SETSIZE; fd++) {
      /* skip if no connection */
      if (!conns[fd])
        continue;

      /* is their activity on their fd? */
      if (FD_ISSET(fd, &rfds)) {
        /* yes! */
        printf("[%d] activity\n", fd);

        /* create a buffer to read into */
        char buf[1024];
        int nread = read(fd, buf, sizeof(buf));

        /* see how much we read */
        if (nread < 0) {
          /* less then zero is some error. disconnect them */
          fprintf(stderr, "read(%d): %s\n", fd, strerror(errno));
          close(fd);
          conns[fd] = 0;
        }

        else if (nread > 0) {
          /* we got some stuff from them! */
          printf("[%d] read: %.*s\n", fd, nread, buf);

          /* loop over all our connections, and send stuff onto them! */
          for (int dest_fd = 0; dest_fd < FD_SETSIZE; dest_fd++) {

            /* take active connections, but not ourselves */
            if (conns[dest_fd] && dest_fd != fd) {

              /* write to them */
              if (write(dest_fd, buf, nread) < 0) {
                /* disconnect if it fails; they might have legitimately gone away without telling us */
                fprintf(stderr, "write(%d): %s\n", dest_fd, strerror(errno));
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
          close(fd);
          conns[fd] = 0;
        }
      }
    }

    /* we've processed all activity, so now we need to set up the descriptor
     * set again (remember, select() removes descriptors that had no activity) */
    FD_ZERO(&rfds);

    /* add the server */
    FD_SET(server_fd, &rfds);
    max_fd = server_fd+1;

    /* and all the active connections */
    for (int fd = 0; fd < FD_SETSIZE; fd++) {
      if(conns[fd]) {
        FD_SET(fd, &rfds);
        max_fd = fd+1;
      }
    }
  }

  /* select failed. in a real server you might actually need to handle
   * non-error cases like EINTR, but it complicates this example so we won't
   * bother */
  perror("select");
  exit(1);
}
