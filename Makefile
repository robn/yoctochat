CFLAGS := -Wall -ggdb

PROGRAMS_SIMPLE := yc_select yc_poll
PROGRAMS_URING :=

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
PROGRAMS_SIMPLE += yc_epoll
PROGRAMS_URING  += yc_uring
endif
ifeq ($(UNAME_S),FreeBSD)
PROGRAMS_SIMPLE += yc_kqueue
endif

all: $(PROGRAMS_SIMPLE) $(PROGRAMS_URING)

$(PROGRAMS_SIMPLE): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

$(PROGRAMS_URING): %: %.c
	$(CC) $(CFLAGS) -o $@ $< -luring

clean:
	rm -f $(PROGRAMS_SIMPLE) $(PROGRAMS_URING)
