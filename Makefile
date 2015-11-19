-include mconfig

objects = dinit.o load_service.o service.o control.o dinit-log.o dinit-start.o

dinit_objects = dinit.o load_service.o service.o control.o dinit-log.o

all: dinit dinit-start

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) -lev $(EXTRA_LIBS)

dinit-start: dinit-start.o
	$(CXX) -o dinit-start dinit-start.o $(EXTRA_LIBS)

$(objects): %.o: %.cc service.h dinit-log.h control.h control-cmds.h
	$(CXX) $(CXXOPTS) -c $< -o $@

#install: all

#install.man:

clean:
	rm *.o
	rm dinit
