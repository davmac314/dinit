-include ../mconfig

objects = dinit.o load_service.o service.o control.o dinit-log.o dinit-start.o shutdown.o dinit-reboot.o

dinit_objects = dinit.o load_service.o service.o control.o dinit-log.o

all: dinit dinit-start

shutdown-utils: shutdown

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) -lev $(EXTRA_LIBS)

dinit-start: dinit-start.o
	$(CXX) -o dinit-start dinit-start.o $(EXTRA_LIBS)
	
shutdown: shutdown.o
	$(CXX) -o shutdown shutdown.o

dinit-reboot: dinit-reboot.o
	$(CXX) -o dinit-reboot dinit-reboot.o	

$(objects): %.o: %.cc service.h dinit-log.h control.h control-cmds.h
	$(CXX) $(CXXOPTS) -c $< -o $@

#install: all

#install.man:

clean:
	rm *.o
	rm dinit
