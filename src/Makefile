include ../mconfig
include ../build/version.conf

STRIPOPTS ?= -s
ALL_CXXFLAGS=$(CPPFLAGS) $(CXXFLAGS) $(CXXFLAGS_EXTRA)
ALL_LDFLAGS=$(LDFLAGS) $(LDFLAGS_EXTRA)

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=$(SHUTDOWN_PREFIX)shutdown
endif

ifeq ($(SUPPORT_CAPABILITIES),1)
  ALL_LDFLAGS+=-lcap
endif

dinit_objects = dinit.o load-service.o service.o proc-service.o baseproc-service.o control.o dinit-log.o \
		dinit-main.o run-child-proc.o options-processing.o dinit-env.o settings.o

objects = $(dinit_objects) dinitctl.o dinitcheck.o shutdown.o dinit-monitor.o

all: dinit dinitctl dinitcheck dinit-monitor $(SHUTDOWN)

../mconfig:
	@echo "*** No mconfig, please run make from top level"
	@false

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(ALL_LDFLAGS)

dinitctl: dinitctl.o options-processing.o settings.o
	$(CXX) -o dinitctl dinitctl.o options-processing.o settings.o $(ALL_LDFLAGS)

dinitcheck: dinitcheck.o options-processing.o settings.o
	$(CXX) -o dinitcheck dinitcheck.o options-processing.o settings.o $(ALL_LDFLAGS)

dinit-monitor: dinit-monitor.o
	$(CXX) -o dinit-monitor dinit-monitor.o $(ALL_LDFLAGS)

$(SHUTDOWN_PREFIX)shutdown: shutdown.o
	$(CXX) -o $(SHUTDOWN_PREFIX)shutdown shutdown.o $(ALL_LDFLAGS)

$(objects): ../build/includes/mconfig.h

$(objects): %.o: %.cc
	$(CXX) $(ALL_CXXFLAGS) -MMD -MP -Iincludes -I../dasynq/include -I../build/includes -c $< -o $@

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
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)soft-reboot
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)poweroff
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl dinitcheck $(SHUTDOWN_PREFIX)shutdown dinit-monitor
	$(MAKE) -C tests clean
	$(MAKE) -C igr-tests clean

-include $(objects:.o=.d)
