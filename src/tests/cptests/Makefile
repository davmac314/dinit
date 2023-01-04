-include ../../../mconfig

objects = cptests.o
parent_test_objects = ../test-bpsys.o ../test-dinit.o ../test-run-child-proc.o
parent_objs = control.o dinit-log.o service.o load-service.o proc-service.o baseproc-service.o dinit-env.o

check: build-tests run-tests

build-tests: cptests

run-tests: cptests
	./cptests

cptests: cptests.o $(parent_objs) $(parent_test_objs)
	$(CXX) -o cptests cptests.o $(parent_test_objects) $(parent_objs) $(CXXOPTS) $(CXXFLAGS) $(LDFLAGS) $(CUSTOM_LDFLAGS) $(SANITIZEOPTS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(CXXFLAGS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes -c $< -o $@

$(parent_objs): %.o: ../../%.cc
	$(CXX) $(CXXOPTS) $(CXXFLAGS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes -c $< -o $@

clean:
	rm -f *.o *.d cptests


# Experimental LLVM-libFuzzer based fuzzer. "make fuzz" to build; "fuzz corpus" to run (and store
# interesting test data in "corpus" directory).

fuzz_parent_test_objects = $(foreach obj,$(notdir $(parent_test_objects)),fuzz-$(obj))
fuzz_objects = $(foreach obj,$(parent_objs),fuzz-$(obj))

# Create mconfig.h for fuzzing. because fuzz target not runned by top level make; we need to create mconfig.h manually.
../../../mconfig:
	$(MAKE) -C ../../../ mconfig

../../../build/includes/mconfig.h: ../../../mconfig
	$(MAKE) -C ../../../build all

fuzz: ../../../build/includes/mconfig.h fuzz.cc $(fuzz_parent_test_objects) $(fuzz_objects)
	clang++ -std=c++11 -g -O1 -Iincludes -I../../../dasynq/include -I../../../build/includes/ -fsanitize=fuzzer,address,undefined,leak fuzz.cc $(fuzz_parent_test_objects) $(fuzz_objects) -o fuzz

$(fuzz_parent_test_objects): fuzz-%.o: ../%.cc
	clang -O1 -fsanitize=address,undefined,fuzzer-no-link,leak -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes/ -c $< -o $@

$(fuzz_objects): fuzz-%.o: ../../%.cc
	clang -O1 -fsanitize=address,undefined,fuzzer-no-link,leak -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes/ -c $< -o $@


-include $(objects:.o=.d)
-include $(parent_objects:.o=.d)
