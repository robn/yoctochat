CFLAGS := -Wall -ggdb

PROGRAMS_SIMPLE := yc_select yc_poll yc_epoll yc_kqueue
PROGRAMS_URING  := yc_uring

all: $(PROGRAMS_SIMPLE) $(PROGRAMS_URING)

$(PROGRAMS_SIMPLE): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

$(PROGRAMS_URING): %: %.c
	$(CC) $(CFLAGS) -o $@ $< -luring

clean:
	rm -f $(PROGRAMS_SIMPLE) $(PROGRAMS_URING)
