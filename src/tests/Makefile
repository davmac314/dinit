-include ../../mconfig

objects = tests.o test-dinit.o proctests.o test-run-child-proc.o test-bpsys.o
parent_objs = service.o proc-service.o dinit-log.o load-service.o baseproc-service.o

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

tests: prepare-incdir $(parent_objs) tests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) $(SANITIZEOPTS) -o tests $(parent_objs) tests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(LDFLAGS)

proctests: prepare-incdir $(parent_objs) proctests.o test-dinit.o test-bpsys.o test-run-child-proc.o
	$(CXX) $(SANITIZEOPTS) -o proctests $(parent_objs) proctests.o test-dinit.o test-bpsys.o test-run-child-proc.o $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../dasynq -c $< -o $@

$(parent_objs): %.o: ../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../dasynq -c $< -o $@

clean:
	rm -f *.o *.d

-include $(objects:.o=.d)
-include $(parent_objs:.o=.d)
