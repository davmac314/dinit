-include ../../mconfig

objects = tests.o test-dinit.o
parent_objs = service.o baseproc-service.o proc-service.o dinit-log.o load_service.o

check: build-tests
	./tests

build-tests: prepare-incdir tests

# Create an "includes" directory populated with a combination of real and mock headers:
prepare-incdir:
	mkdir -p includes
	rm -rf includes/*.h
	cd includes; ln -sf ../../includes/*.h .
	cd includes; ln -sf ../test-includes/*.h .

tests: $(objects) $(parent_objs)
	$(CXX) $(SANITIZEOPTS) -o tests $(objects) $(parent_objs) $(EXTRA_LIBS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -Iincludes -I../dasynq -c $< -o $@

$(parent_objs): %.o: ../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -Iincludes -I../dasynq -c $< -o $@

clean:
	rm -f *.o *.d

$(objects:.o=.d): %.d: %.cc
	$(CXX) $(CXXOPTS) -Iincludes -I../dasynq -MM -MG -MF $@ $<

include $(objects:.o=.d)
