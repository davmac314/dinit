-include mconfig

objects = dinit.o load_service.o service.o dinit-start.o

dinit_objects = dinit.o load_service.o service.o

all: dinit dinit-start

dinit: $(dinit_objects)
	$(CXX) -o dinit $(dinit_objects) -lev $(EXTRA_LIBS)

dinit-start: dinit-start.o
	$(CXX) -o dinit-start dinit-start.o $(EXTRA_LIBS)

$(objects): %.o: %.cc service.h
	$(CXX) -D_GLIBCXX_USE_CXX11_ABI=0 -std=gnu++11 -c -Os -Wall $< -o $@

#install: all

#install.man:

clean:
	rm *.o
	rm dinit
