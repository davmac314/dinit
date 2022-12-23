-include ../../mconfig

objects = tests.o test-dinit.o proctests.o loadtests.o envtests.o test-run-child-proc.o test-bpsys.o
parent_objs = service.o proc-service.o dinit-log.o load-service.o baseproc-service.o dinit-env.o

check: build-tests run-tests

build-tests: prepare-incdir tests proctests loadtests envtests
	$(MAKE) -C cptests build-tests

run-tests: tests proctests loadtests envtests
	./tests
	./proctests
	./loadtests
	./envtests
	$(MAKE) -C cptests run-tests

# Create an "includes" directory populated with a combination of real and mock headers:
prepare-incdir:
	mkdir -p includes
	rm -rf includes/*.h
	cd includes; ln -f ../../includes/*.h .
	cd includes; ln -f ../test-includes/*.h .

tests: $(parent_objs) tests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o tests $(parent_objs) tests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(CXXOPTS) $(LDFLAGS) $(SANITIZEOPTS)

proctests: $(parent_objs) proctests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o proctests $(parent_objs) proctests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(CXXOPTS) $(LDFLAGS) $(SANITIZEOPTS)
	
loadtests: $(parent_objs) loadtests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o loadtests $(parent_objs) loadtests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(CXXOPTS) $(LDFLAGS) $(SANITIZEOPTS)

envtests: $(parent_objs) envtests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o envtests $(parent_objs) envtests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(CXXOPTS) $(LDFLAGS) $(SANITIZEOPTS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../dasynq/include -I../../build/includes -c $< -o $@

$(parent_objs): %.o: ../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../dasynq/include -I../../build/includes -c $< -o $@

clean:
	$(MAKE) -C cptests clean
	rm -f *.o *.d tests proctests loadtests

-include $(objects:.o=.d)
-include $(parent_objs:.o=.d)
