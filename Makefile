# Makefile for Dinit.

all:
	$(MAKE) -C src all

check:
	$(MAKE) -C src check

run-cppcheck:
	$(MAKE) -C src run-cppcheck

install:
	$(MAKE) -C src install
	$(MAKE) -C doc/manpages install

clean:
	$(MAKE) -C src clean
