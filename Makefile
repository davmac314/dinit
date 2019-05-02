# Makefile for Dinit.

all:
	$(MAKE) -C src all
	@echo "***"
	@echo "*** Build complete; use \"make check\" to run tests or \"make install\" to install."
	@echo "***"

check:
	$(MAKE) -C src check

run-cppcheck:
	$(MAKE) -C src run-cppcheck

install:
	$(MAKE) -C src install
	$(MAKE) -C doc/manpages install

clean:
	$(MAKE) -C src clean
