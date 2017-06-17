-include ../../mconfig

objects = tests.o
parent_objs = ../service.o ../control.o ../dinit-log.o ../load_service.o ../dinit.o

check: build-tests
	./tests

build-tests: tests

tests: $(objects) $(parent_objs)
	$(CXX) -o tests tests.o $(parent_objs) $(EXTRA_LIBS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) -I.. -I../dasynq -c $< -o $@

clean:
	rm -f *.o *.d

$(objects:.o=.d): %.d: %.cc
	$(CXX) $(CXXOPTS) -I.. -I../dasynq -MM -MG -MF $@ $<

include $(objects:.o=.d)
