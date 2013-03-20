# Very simple Makefile

CFLAGS = -W -Wall -O3

disk-filltest: disk-filltest.c
	$(CC) $(CFLAGS) -o disk-filltest disk-filltest.c
