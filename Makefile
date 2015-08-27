-include mconfig

objects = dinit.o load_service.o service.o dinit-start.o

dinit_objects = dinit.o load_service.o service.o

all: dinit dinit-start

dinit: $(dinit_objects)
	g++ -Wall -o dinit $(dinit_objects) -lev

dinit-start: dinit-start.o
	g++ -Wall -o dinit-start dinit-start.o

# Note we use the old ABI on GCC 5.2 to avoid GCC bug 66145.
$(objects): %.o: %.cc service.h
	g++ -D_GLIBCXX_USE_CXX11_ABI=0 -std=gnu++11 -c -Os -Wall $< -o $@

install: all
	#install -d $(LOGINBINDIR) $(LOGINDOCDIR)
	#install -s login $(LOGINBINDIR)
	#install --mode=644 README $(LOGINDOCDIR)
	#@echo
	#@echo "You may also wish to \"make install.man\"."

install.man:
	#install -d $(MAN1DIR)
	#install --mode=644 login.1 $(MAN1DIR)

clean:
	rm *.o
	rm dinit
