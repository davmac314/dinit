include ../mconfig
include ../build/version.conf

STRIPOPTS ?= -s
ALL_CXXFLAGS=$(CPPFLAGS) $(CXXFLAGS) $(CXXFLAGS_EXTRA)
ALL_LDFLAGS=$(LDFLAGS) $(LDFLAGS_EXTRA)

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=$(SHUTDOWN_PREFIX)shutdown
endif

dinit_objects = dinit.o load-service.o service.o proc-service.o baseproc-service.o control.o dinit-log.o \
		dinit-main.o run-child-proc.o options-processing.o dinit-env.o settings.o dinit-iostream.o

objects = $(dinit_objects) dinitctl.o dinit-check.o shutdown.o dinit-monitor.o dinit-client.o

all: dinit dinitctl dinit-check dinit-monitor $(SHUTDOWN)

../mconfig:
	@echo "*** No mconfig, please run make from top level"
	@false

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(ALL_LDFLAGS) $(LDFLAGS_LIBCAP)

dinitctl: dinitctl.o options-processing.o settings.o dinit-iostream.o dinit-client.o
	$(CXX) -o dinitctl dinitctl.o options-processing.o settings.o dinit-iostream.o dinit-client.o $(ALL_LDFLAGS)

dinit-check: dinit-check.o options-processing.o settings.o dinit-iostream.o dinit-client.o
	$(CXX) -o dinit-check dinit-check.o options-processing.o settings.o dinit-iostream.o dinit-client.o $(ALL_LDFLAGS) $(LDFLAGS_LIBCAP)

dinit-monitor: dinit-monitor.o
	$(CXX) -o dinit-monitor dinit-monitor.o $(ALL_LDFLAGS)

$(SHUTDOWN_PREFIX)shutdown: shutdown.o
	$(CXX) -o $(SHUTDOWN_PREFIX)shutdown shutdown.o $(ALL_LDFLAGS)

$(objects): ../build/includes/mconfig.h

$(objects): %.o: %.cc
	$(CXX) $(ALL_CXXFLAGS) $(CPPFLAGS_LIBCAP) -MMD -MP -Iincludes -I../dasynq/include -I../build/includes -c $< -o $@

check: ../build/includes/mconfig.h $(dinit_objects)
	$(MAKE) -C tests check

check-igr: dinit dinitctl dinit-check
	$(MAKE) -C igr-tests check-igr

run-cppcheck:
	cppcheck --std=c++11 -Iincludes -Idasynq --force --enable=all *.cc 2>../cppcheck-report.txt

install: all
	install -d $(DESTDIR)$(BINDIR)
	install $(STRIPOPTS) dinit dinitctl dinit-check dinit-monitor $(DESTDIR)$(BINDIR)
ifeq ($(BUILD_SHUTDOWN),yes)
	install -d $(DESTDIR)$(SBINDIR)
	install $(STRIPOPTS) $(SHUTDOWN) $(DESTDIR)$(SBINDIR)
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)halt
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)reboot
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)soft-reboot
	ln -sf $(SHUTDOWN) $(DESTDIR)$(SBINDIR)/$(SHUTDOWN_PREFIX)poweroff
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl dinit-check $(SHUTDOWN_PREFIX)shutdown dinit-monitor
	$(MAKE) -C tests clean
	$(MAKE) -C igr-tests clean

-include $(objects:.o=.d)
