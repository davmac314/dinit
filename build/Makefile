-include ../mconfig
-include version.conf

all: ../mconfig includes/mconfig.h

# Look for a suitable build config file and use it.
../mconfig:
	@UNAME=`uname`;\
	if [ -f "../configs/mconfig.$$UNAME.sh" ]; then \
	    echo "Found auto-configuration script for OS: $$UNAME"; \
	    ( cd ../configs; sh "mconfig.$$UNAME.sh" ) \
	elif [ -f "../configs/mconfig.$$UNAME" ]; then \
	    echo "Found configuration for OS: $$UNAME"; \
	    ln -sf "configs/mconfig.$$UNAME" ../mconfig; \
	else \
	    echo "No config available. Please create suitable mconfig file."; \
	    exit 1; \
	fi

includes/mconfig.h: ../mconfig tools/mconfig-gen.cc
	$(MAKE) -C tools mconfig-gen
	./tools/mconfig-gen SBINDIR=$(SBINDIR) SYSCONTROLSOCKET=$(SYSCONTROLSOCKET) \
		SHUTDOWN_PREFIX=$(SHUTDOWN_PREFIX) VERSION=$(VERSION) \
		$(if $(USE_UTMPX),USE_UTMPX=$(USE_UTMPX),) > includes/mconfig.h
