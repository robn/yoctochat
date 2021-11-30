/* yc_kqueue - a yoctochat server using a BSD kqueue IO loop */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
/* max number of connections. in a real program you probably wouldn't do this,
 * and instead use a more dynamic structure for tracking connections */
#define NUM_CONNS (128)
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
            .sin_port = htons(port),
            .sin_addr = {
                    .s_addr = htonl(INADDR_ANY)}};

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

    int conns[NUM_CONNS];
    memset(&conns, 0, sizeof(conns));
    // Prepare the kqueue.
    int kq = kqueue();


    int new_events;

    struct kevent change_event[4], event[4];

    // Create event 'filter', these are the events we want to monitor.
    // Here we want to monitor: socket_listen_fd, for the events: EVFILT_READ
    // (when there is data to be read on the socket), and perform the following
    // actions on this kevent: EV_ADD and EV_ENABLE (add the event to the kqueue
    // and enable it).
    EV_SET(change_event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);

    // Register kevent with the kqueue.
    if (kevent(kq, change_event, 1, NULL, 0, NULL) == -1) {
        perror("kevent");
        exit(1);
    }

    for (;;) {
        // Check for new events, but do not register new events with
        // the kqueue. Hence the 2nd and 3rd arguments are NULL, 0.
        // Only handle 1 new event per iteration in the loop; 5th
        // argument is 1.
        new_events = kevent(kq, NULL, 0, event, 1, NULL);
        if (new_events == -1) {
            perror("kevent");
            exit(1);
        }

        for (int i = 0; new_events > i; i++) {
            int event_fd = event[i].ident;

            // When the client disconnects an EOF is sent. By closing the file
            // descriptor the event is automatically removed from the kqueue.
            if (event[i].flags & EV_EOF) {
                printf("Client has disconnected");
                close(event_fd);
            }
            // If the new event's file descriptor is the same as the listening
            // socket's file descriptor, we are sure that a new client wants
            // to connect to our socket.
            else if (event_fd == server_fd) {

                socklen_t sinlen = sizeof(sin);
                // Incoming socket connection on the listening socket.
                // Create a new socket for the actual connection to client.
                int new_fd = accept(event_fd, (struct sockaddr *) &sin, &sinlen);
                if (new_fd < 0) {
                    perror("accept");
                } else {
                    printf("[%d] connect from %s:%d\n", new_fd, inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
                    // Put this new socket connection also as a 'filter' event
                    // to watch in kqueue, so we can now watch for events on this
                    // new socket.
                    EV_SET(change_event, new_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                    if (kevent(kq, change_event, 1, NULL, 0, NULL) < 0) {
                        perror("kevent error");
                    }
                    /* remember our new connection. in a real server, you'd create a
                     * connection or user object of some sort, maybe send them a greeting,
                     * begin authentication, etc */
                    conns[new_fd] = 1;
                }
            }

            else if (event[i].filter & EVFILT_READ) {
                // Read bytes from socket
                char buf[1024];
                size_t nread = recv(event_fd, buf, sizeof(buf), 0);
                printf("read %zu bytes\n", nread);
                /* see how much we read */
                if (nread < 0) {
                    /* less then zero is some error. disconnect them */
                    fprintf(stderr, "read(%d): %s\n", event_fd, strerror(errno));
                    close(event_fd);
                    conns[event_fd] = 0;
                }

                else if (nread > 0) {
                    /* we got some stuff from them! */
                    printf("[%d] read: %.*s\n", event_fd, nread, buf);

                    /* loop over all our connections, and send stuff onto them! */
                    for (int dest_fd = 0; dest_fd < NUM_CONNS; dest_fd++) {

                        /* take active connections, but not ourselves */
                        if (conns[dest_fd] && dest_fd != event_fd) {

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
            }
        }
    }

    /* select failed. in a real server you might actually need to handle
   * non-error cases like EINTR, but it complicates this example so we won't
   * bother */
    perror("kevent");
    exit(1);
}
