-include ../../mconfig

objects = tests.o test-dinit.o proctests.o test-run-child-proc.o
parent_objs = service.o proc-service.o dinit-log.o load_service.o baseproc-service.o

check: build-tests
	./tests
	./proctests

build-tests: tests proctests

# Create an "includes" directory populated with a combination of real and mock headers:
prepare-incdir:
	mkdir -p includes
	rm -rf includes/*.h
	cd includes; ln -sf ../../includes/*.h .
	cd includes; ln -sf ../test-includes/*.h .

tests: prepare-incdir $(parent_objs) tests.o test-dinit.o
	$(CXX) $(SANITIZEOPTS) -o tests $(parent_objs) tests.o test-dinit.o $(EXTRA_LIBS)

proctests: prepare-incdir $(parent_objs) proctests.o test-dinit.o
	$(CXX) $(SANITIZEOPTS) -o proctests $(parent_objs) proctests.o test-dinit.o $(EXTRA_LIBS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -Iincludes -I../dasynq -c $< -o $@

$(parent_objs): %.o: ../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -Iincludes -I../dasynq -c $< -o $@

clean:
	rm -f *.o *.d

$(objects:.o=.d): %.d: %.cc
	$(CXX) $(CXXOPTS) -Iincludes -I../dasynq -MM -MG -MF $@ $<

include $(objects:.o=.d)
