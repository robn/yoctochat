/* yc_poll - a yoctochat server using a classic poll() IO loop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <errno.h>

/* number of pollfds in our array. because of the wey we're implementing this,
 * that's roughly the maximum number of connections we can handle. */
#define NUM_POLLFDS (128)

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

  /* create an array of pollfd structs. each one carries a file descriptor, a
   * set of wanted events, and after the poll() call, a set of events that
   * occurred. for our own convenience we keep a static list and use the file
   * descriptor as the index. */
  struct pollfd pollfds[NUM_POLLFDS];
  for (int fd = 0; fd < NUM_POLLFDS; fd++) {
    /* setting -1 fd disables */
    pollfds[fd].fd = -1;
  }

  /* add the server socket, and request read/input events */
  pollfds[server_fd].fd     = server_fd;
  pollfds[server_fd].events = POLLIN;

  /* wait forever for something to happen */
  while (poll(pollfds, NUM_POLLFDS, -1) >= 0) {

    /* if the server socket has activity, someone connected */
    if (pollfds[server_fd].revents & POLLIN) {
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

        /* enable the pollfd for this fd, and request read events */
        pollfds[new_fd].fd     = new_fd;
        pollfds[new_fd].events = POLLIN;
      }
    }

    for (int fd = 0; fd < NUM_POLLFDS; fd++) {
      if (!(pollfds[fd].revents & POLLIN) || fd == server_fd)
        continue;

      printf("[%d] activity\n", fd);

      /* create a buffer to read into */
      char buf[1024];
      int nread = read(fd, buf, sizeof(buf));

      /* see how much we read */
      if (nread < 0) {
        /* less then zero is some error. disconnect them */
        fprintf(stderr, "read(%d): %s\n", fd, strerror(errno));
        close(fd);
        pollfds[fd].fd = -1;
      }

      else if (nread > 0) {
        /* we got some stuff from them! */
        printf("[%d] read: %.*s\n", fd, nread, buf);

        /* loop over all our connections, and send stuff onto them! */
        for (int dest_fd = 0; dest_fd < NUM_POLLFDS; dest_fd++) {

          /* take active connections, but not ourselves */
          if (pollfds[dest_fd].fd >= 0 && pollfds[dest_fd].fd != fd && pollfds[dest_fd].fd != server_fd) {
            /* write to them */
            if (write(dest_fd, buf, nread) < 0) {
              /* disconnect if it fails; they might have legitimately gone away without telling us */
              fprintf(stderr, "write(%d): %s\n", dest_fd, strerror(errno));
              close(dest_fd);
              pollfds[dest_fd].fd = -1;
            }
          }
        }
      }

      /* zero byes read */
      else {
        /* so they gracefully disconnected and we should forget them */
        printf("[%d] closed\n", fd);
        close(fd);
        pollfds[fd].fd = -1;
      }
    }
  }

  /* poll failed. in a real server you might actually need to handle
   * non-error cases like EINTR, but it complicates this example so we won't
   * bother */
  perror("poll");
  exit(1);
}
