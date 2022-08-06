include ../mconfig
include ../build/version.conf

STRIPOPTS ?= -s

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=$(SHUTDOWN_PREFIX)shutdown
endif

dinit_objects = dinit.o load-service.o service.o proc-service.o baseproc-service.o control.o dinit-log.o \
		dinit-main.o run-child-proc.o options-processing.o dinit-env.o

objects = $(dinit_objects) dinitctl.o dinitcheck.o shutdown.o dinit-monitor.o

all: dinit dinitctl dinitcheck dinit-monitor $(SHUTDOWN)

../mconfig:
	@echo "*** No mconfig, please run make from top level"
	@false

$(objects): ../build/includes/mconfig.h

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(CXXOPTS) $(LDFLAGS)

dinitctl: dinitctl.o
	$(CXX) -o dinitctl dinitctl.o $(CXXOPTS) $(LDFLAGS)

dinitcheck: dinitcheck.o options-processing.o
	$(CXX) -o dinitcheck dinitcheck.o options-processing.o $(CXXOPTS) $(LDFLAGS)

dinit-monitor: dinit-monitor.o
	$(CXX) -o dinit-monitor dinit-monitor.o $(CXXOPTS) $(LDFLAGS)

$(SHUTDOWN_PREFIX)shutdown: shutdown.o
	$(CXX) -o $(SHUTDOWN_PREFIX)shutdown shutdown.o $(CXXOPTS) $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) -MMD -MP -Iincludes -I../dasynq/include -I../build/includes -c $< -o $@

check: ../build/includes/mconfig.h $(dinit_objects)
	$(MAKE) -C tests check

check-igr: dinit dinitctl dinitcheck
	$(MAKE) -C igr-tests check-igr

run-cppcheck:
	cppcheck --std=c++11 -Iincludes -Idasynq --force --enable=all *.cc 2>../cppcheck-report.txt

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install $(STRIPOPTS) dinit dinitctl dinitcheck dinit-monitor $(SHUTDOWN) $(DESTDIR)$(SBINDIR)
ifeq ($(BUILD_SHUTDOWN),yes)
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)halt
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)reboot
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)poweroff
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl dinitcheck shutdown
	$(MAKE) -C tests clean
	$(MAKE) -C igr-tests clean

-include $(objects:.o=.d)
