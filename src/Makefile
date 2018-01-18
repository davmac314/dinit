include ../mconfig

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=shutdown
endif

dinit_objects = dinit.o load_service.o service.o proc-service.o baseproc-service.o control.o dinit-log.o \
		dinit-main.o run-child-proc.o

objects = $(dinit_objects) dinitctl.o shutdown.o

all: dinit dinitctl $(SHUTDOWN)

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(LDFLAGS)

dinitctl: dinitctl.o
	$(CXX) -o dinitctl dinitctl.o $(LDFLAGS)
	
shutdown: shutdown.o
	$(CXX) -o shutdown shutdown.o $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) -MMD -MP -Iincludes -Idasynq -c $< -o $@

check:
	$(MAKE) -C tests check

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -s dinit dinitctl $(SHUTDOWN) $(DESTDIR)$(SBINDIR)
ifeq ($(BUILD_SHUTDOWN),yes)
	install halt reboot $(DESTDIR)$(SBINDIR)
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl shutdown
	$(MAKE) -C tests clean

-include $(objects:.o=.d)
