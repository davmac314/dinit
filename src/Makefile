include ../mconfig
include ../build/version.conf

STRIPOPTS ?= -s

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=$(SHUTDOWN_PREFIX)shutdown
endif

dinit_objects = dinit.o load-service.o service.o proc-service.o baseproc-service.o control.o dinit-log.o \
		dinit-main.o run-child-proc.o options-processing.o

objects = $(dinit_objects) dinitctl.o dinitcheck.o shutdown.o

all: dinit dinitctl dinitcheck $(SHUTDOWN)

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

mconfig-gen: mconfig-gen.cc ../mconfig
	$(HOSTCXX) $(HOSTCXXOPTS) -o mconfig-gen mconfig-gen.cc $(HOSTLDFLAGS)

$(objects): ../build/includes/mconfig.h

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(LDFLAGS)

dinitctl: dinitctl.o
	$(CXX) -o dinitctl dinitctl.o $(LDFLAGS)

dinitcheck: dinitcheck.o options-processing.o
	$(CXX) -o dinitcheck dinitcheck.o options-processing.o $(LDFLAGS)

$(SHUTDOWNPREFIX)shutdown: shutdown.o
	$(CXX) -o $(SHUTDOWNPREFIX)shutdown shutdown.o $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) -MMD -MP -Iincludes -Idasynq -I../build/includes -c $< -o $@

check: ../build/includes/mconfig.h
	$(MAKE) -C tests check

check-igr: dinit dinitctl dinitcheck
	$(MAKE) -C igr-tests check-igr

run-cppcheck:
	cppcheck --std=c++11 -Iincludes -Idasynq --force --enable=all *.cc 2>../cppcheck-report.txt

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install $(STRIPOPTS) dinit dinitctl dinitcheck $(SHUTDOWN) $(DESTDIR)$(SBINDIR)
ifeq ($(BUILD_SHUTDOWN),yes)
	ln -f $(DESTDIR)$(SBINDIR)/$(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWNPREFIX)halt
	ln -f $(DESTDIR)$(SBINDIR)/$(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWNPREFIX)reboot
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl dinitcheck shutdown mconfig-gen
	rm -f includes/mconfig.h
	$(MAKE) -C tests clean
	$(MAKE) -C igr-tests clean

-include $(objects:.o=.d)
