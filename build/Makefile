-include ../mconfig
-include version.conf

all: ../mconfig includes/mconfig.h

../mconfig:
	@echo "*** No mconfig, please run make from top level"
	@false

includes/mconfig.h: ../mconfig tools/mconfig-gen.cc version.conf
	$(MAKE) -C tools mconfig-gen
	./tools/mconfig-gen BINDIR=$(BINDIR) SBINDIR=$(SBINDIR) SYSCONTROLSOCKET=$(SYSCONTROLSOCKET) \
		SHUTDOWN_PREFIX=$(SHUTDOWN_PREFIX) VERSION=$(VERSION) \
		DEFAULT_AUTO_RESTART=$(DEFAULT_AUTO_RESTART) \
		DEFAULT_START_TIMEOUT=$(DEFAULT_START_TIMEOUT) \
		DEFAULT_STOP_TIMEOUT=$(DEFAULT_STOP_TIMEOUT) \
		$(if $(SUPPORT_CGROUPS),SUPPORT_CGROUPS=$(SUPPORT_CGROUPS),) \
		$(if $(SUPPORT_CAPABILITIES),SUPPORT_CAPABILITIES=$(SUPPORT_CAPABILITIES),) \
		$(if $(SUPPORT_IOPRIO),SUPPORT_IOPRIO=$(SUPPORT_IOPRIO),) \
		$(if $(SUPPORT_OOM_ADJ),SUPPORT_OOM_ADJ=$(SUPPORT_OOM_ADJ),) \
		$(if $(USE_UTMPX),USE_UTMPX=$(USE_UTMPX),) \
		$(if $(USE_INITGROUPS),USE_INITGROUPS=$(USE_INITGROUPS),) > includes/mconfig.h

clean:
	rm -f includes/mconfig.h
	$(MAKE) -C tools clean
