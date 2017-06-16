-include ../mconfig

ifeq ($(BUILD_SHUTDOWN),yes)
  SHUTDOWN=shutdown
endif

objects = dinit.o load_service.o service.o control.o dinit-log.o dinit-main.o dinitctl.o shutdown.o

dinit_objects = dinit.o load_service.o service.o control.o dinit-log.o dinit-main.o

all: dinit dinitctl $(SHUTDOWN)

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) $(EXTRA_LIBS)

dinitctl: dinitctl.o
	$(CXX) -o dinitctl dinitctl.o $(EXTRA_LIBS)
	
shutdown: shutdown.o
	$(CXX) -o shutdown shutdown.o

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) -Idasynq -c $< -o $@

check: $(objects)
	$(MAKE) -C tests check

install: all
	install -d $(DESTDIR)/sbin
	install -s dinit dinitctl $(SHUTDOWN) $(DESTDIR)/sbin
ifeq ($(BUILD_SHUTDOWN),yes)
	install halt reboot $(DESTDIR)/sbin
endif

clean:
	rm -f *.o *.d
	rm -f dinit dinitctl shutdown
	$(MAKE) -C tests clean

$(objects:.o=.d): %.d: %.cc
	$(CXX) $(CXXOPTS) -Idasynq -MM -MG -MF $@ $<

include $(objects:.o=.d)
