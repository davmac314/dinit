-include ../../mconfig

ALL_TEST_CXXFLAGS=$(CPPFLAGS) $(TEST_CXXFLAGS) $(TEST_CXXFLAGS_EXTRA)
ALL_TEST_LDFLAGS=$(TEST_LDFLAGS) $(TEST_LDFLAGS_EXTRA)

ifeq ($(SUPPORT_CAPABILITIES),1)
  ALL_TEST_LDFLAGS+=-lcap
endif

objects = tests.o test-dinit.o proctests.o loadtests.o envtests.o test-run-child-proc.o test-bpsys.o
parent_objs = service.o proc-service.o dinit-log.o load-service.o baseproc-service.o dinit-env.o control.o settings.o

check: build-tests run-tests

build-tests: tests proctests loadtests envtests
	$(MAKE) -C cptests build-tests

run-tests: build-tests
	./tests
	./proctests
	./loadtests
	./envtests
	$(MAKE) -C cptests run-tests

tests: $(parent_objs) tests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o tests $(parent_objs) tests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(ALL_TEST_LDFLAGS)

proctests: $(parent_objs) proctests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o proctests $(parent_objs) proctests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(ALL_TEST_LDFLAGS)
	
loadtests: $(parent_objs) loadtests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o loadtests $(parent_objs) loadtests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(ALL_TEST_LDFLAGS)

envtests: $(parent_objs) envtests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) -o envtests $(parent_objs) envtests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(ALL_TEST_LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(ALL_TEST_CXXFLAGS) -MMD -MP -Itest-includes -I../../dasynq/include -I../../build/includes -I../includes -c $< -o $@

$(parent_objs): %.o: ../%.cc
	$(CXX) $(ALL_TEST_CXXFLAGS) -MMD -MP -Itest-includes -I../../dasynq/include -I../../build/includes -I../includes -c $< -o $@

clean:
	$(MAKE) -C cptests clean
	rm -f *.o *.d tests proctests loadtests envtests

-include $(objects:.o=.d)
-include $(parent_objs:.o=.d)
