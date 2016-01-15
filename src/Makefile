-include ../mconfig

objects = dinit.o load_service.o service.o control.o dinit-log.o dinitctl.o shutdown.o dinit-reboot.o

dinit_objects = dinit.o load_service.o service.o control.o dinit-log.o

all: dinit dinitctl

shutdown-utils: shutdown

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) -lev $(EXTRA_LIBS)

dinitctl: dinitctl.o
	$(CXX) -o dinitctl dinitctl.o $(EXTRA_LIBS)
	
shutdown: shutdown.o
	$(CXX) -o shutdown shutdown.o

dinit-reboot: dinit-reboot.o
	$(CXX) -o dinit-reboot dinit-reboot.o	

$(objects): %.o: %.cc service.h dinit-log.h control.h control-cmds.h cpbuffer.h
	$(CXX) $(CXXOPTS) -c $< -o $@

#install: all

#install.man:

clean:
	rm -f *.o
	rm -f dinit dinitctl shutdown
