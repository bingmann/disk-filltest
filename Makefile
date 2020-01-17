# Very Simple Makefile

CFLAGS = -W -Wall -O3 -ansi

# Directories for executables and manuals
prefix = $(DESTDIR)/usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
man1dir = $(prefix)/man1

GZIP = gzip
INSTALL = install
RM = rm

all: disk-filltest

disk-filltest: disk-filltest.c
	$(CC) $(CFLAGS) -o disk-filltest disk-filltest.c

disk-filltest.1.gz: disk-filltest.1
	$(GZIP) -9k disk-filltest.1

install: install-bin install-man

install-bin: disk-filltest
	$(INSTALL) -m 0755 -t $(bindir) -D disk-filltest

install-man: disk-filltest.1.gz
	$(INSTALL) -m 0644 -t  $(man1dir) -D disk-filltest.1.gz

clean:
	$(RM) -f disk-filltest
