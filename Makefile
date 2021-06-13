CFLAGS := -Wall -ggdb

PROGRAMS := yc_select yc_poll

all: $(PROGRAMS)

$(PROGRAMS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMS)
