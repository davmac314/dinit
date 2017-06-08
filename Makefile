# Makefile for Dinit.

all:
	$(MAKE) -C src all

install:
	$(MAKE) -C src install
	$(MAKE) -C doc/manpages install

clean:
	$(MAKE) -C src clean
