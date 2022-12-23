-include ../../../mconfig

objects = cptests.o
parent_test_objects = ../test-bpsys.o ../test-dinit.o ../test-run-child-proc.o
parent_objs = control.o dinit-log.o service.o load-service.o proc-service.o baseproc-service.o dinit-env.o

check: build-tests run-tests

build-tests: prepare-incdir cptests

run-tests: cptests
	./cptests
	
# Create an "includes" directory populated with a combination of real and mock headers:
prepare-incdir:
	mkdir -p includes
	rm -rf includes/*.h
	cd includes; ln -f ../../../includes/*.h .
	cd includes; ln -f ../../test-includes/dinit.h .
	cd includes; ln -f ../../test-includes/baseproc-sys.h .

cptests: cptests.o $(parent_objs) $(parent_test_objs)
	$(CXX) -o cptests cptests.o $(parent_test_objects) $(parent_objs) $(CXXOPTS) $(LDFLAGS) $(SANITIZEOPTS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes -c $< -o $@

$(parent_objs): %.o: ../../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes -c $< -o $@

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

fuzz: prepare-incdir ../../../build/includes/mconfig.h fuzz.cc $(fuzz_parent_test_objects) $(fuzz_objects)
	clang++ -std=c++11 -g -O1 -Iincludes -I../../../dasynq/include -I../../../build/includes/ -fsanitize=fuzzer,address,undefined,leak fuzz.cc $(fuzz_parent_test_objects) $(fuzz_objects) -o fuzz

$(fuzz_parent_test_objects): fuzz-%.o: ../%.cc
	clang -O1 -fsanitize=address,undefined,fuzzer-no-link,leak -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes/ -c $< -o $@

$(fuzz_objects): fuzz-%.o: ../../%.cc
	clang -O1 -fsanitize=address,undefined,fuzzer-no-link,leak -MMD -MP -Iincludes -I../../../dasynq/include -I../../../build/includes/ -c $< -o $@


-include $(objects:.o=.d)
-include $(parent_objects:.o=.d)
