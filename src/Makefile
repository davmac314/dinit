include ../mconfig

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=shutdown
endif

dinit_objects = dinit.o load-service.o service.o proc-service.o baseproc-service.o control.o dinit-log.o \
		dinit-main.o run-child-proc.o

objects = $(dinit_objects) dinitctl.o shutdown.o

all: dinit dinitctl $(SHUTDOWN)

includes/mconfig.h: mconfig-gen
	./mconfig-gen SBINDIR=$(SBINDIR) SYSCONTROLSOCKET=$(SYSCONTROLSOCKET) > includes/mconfig.h

mconfig-gen: mconfig-gen.cc ../mconfig
	$(CXX) $(CXXOPTS) -o mconfig-gen mconfig-gen.cc $(LDFLAGS)

dinit: includes/mconfig.h $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(LDFLAGS)

dinitctl: dinitctl.o
	$(CXX) -o dinitctl dinitctl.o $(LDFLAGS)
	
shutdown: shutdown.o
	$(CXX) -o shutdown shutdown.o $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) -MMD -MP -Iincludes -Idasynq -c $< -o $@

check:
	$(MAKE) -C tests check

run-cppcheck:
	cppcheck --std=c++11 -Iincludes -Idasynq --force --enable=all *.cc 2>../cppcheck-report.txt

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -s dinit dinitctl $(SHUTDOWN) $(DESTDIR)$(SBINDIR)
ifeq ($(BUILD_SHUTDOWN),yes)
	ln $(DESTDIR)$(SBINDIR)\shutdown $(DESTDIR)$(SBINDIR)\halt
	ln $(DESTDIR)$(SBINDIR)\shutdown $(DESTDIR)$(SBINDIR)\reboot
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl shutdown mconfig-gen
	rm -f includes/mconfig.h
	$(MAKE) -C tests clean

-include $(objects:.o=.d)
