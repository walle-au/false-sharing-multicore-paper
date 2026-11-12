CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic

mesi_sim: mesi_sim.c
	$(CC) $(CFLAGS) -o $@ $<

sweep: mesi_sim
	./sweep.sh

clean:
	rm -f mesi_sim

.PHONY: sweep clean
