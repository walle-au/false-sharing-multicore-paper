CC      ?= gcc
CFLAGS  ?= -O2 -g -pthread -Wall -Wextra

all: fs_bench

fs_bench: fs_bench.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f fs_bench fs fs_atomic

.PHONY: all clean
