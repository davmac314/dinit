-include ../mconfig
-include version.conf

all: ../mconfig includes/mconfig.h

../mconfig:
	@echo "*** No mconfig, please run make from top level"
	@false

includes/mconfig.h: ../mconfig tools/mconfig-gen.cc
	$(MAKE) -C tools mconfig-gen
	./tools/mconfig-gen SBINDIR=$(SBINDIR) SYSCONTROLSOCKET=$(SYSCONTROLSOCKET) \
		SHUTDOWN_PREFIX=$(SHUTDOWN_PREFIX) VERSION=$(VERSION) \
		$(if $(SUPPORT_CGROUPS),SUPPORT_CGROUPS=$(SUPPORT_CGROUPS),) \
		$(if $(USE_UTMPX),USE_UTMPX=$(USE_UTMPX),) > includes/mconfig.h

clean:
	rm -f includes/mconfig.h
	$(MAKE) -C tools clean
