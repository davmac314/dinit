-include ../../mconfig

objects = tests.o test-dinit.o
parent_objs = service.o baseproc-service.o proc-service.o control.o dinit-log.o load_service.o # dinit.o

check: build-tests
	./tests

build-tests: tests

tests: $(objects) $(parent_objs)
	$(CXX) $(SANITIZEOPTS) -o tests $(objects) $(parent_objs) $(EXTRA_LIBS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -I.. -I../dasynq -c $< -o $@

$(parent_objs): %.o: ../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -I.. -I../dasynq -c $< -o $@

clean:
	rm -f *.o *.d

$(objects:.o=.d): %.d: %.cc
	$(CXX) $(CXXOPTS) -I.. -I../dasynq -MM -MG -MF $@ $<

include $(objects:.o=.d)
