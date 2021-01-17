# Makefile for Dinit.

all:
	$(MAKE) -C build all
	$(MAKE) -C src all
	$(MAKE) -C doc/manpages all
	@echo "***"
	@echo "*** Build complete; use \"make check\" to run unit tests, \"make check-igr\" for"
	@echo "*** integration tests, or \"make install\" to install."
	@echo "***"

check:
	$(MAKE) -C src check

check-igr:
	$(MAKE) -C src check-igr

run-cppcheck:
	$(MAKE) -C src run-cppcheck

install:
	$(MAKE) -C src install
	$(MAKE) -C doc/manpages install

clean:
	$(MAKE) -C src clean
	$(MAKE) -C doc/manpages clean
