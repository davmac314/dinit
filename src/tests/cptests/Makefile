-include ../../../mconfig

objects = cptests.o
parent_test_objects = ../test-bpsys.o ../test-dinit.o
parent_objs = control.o dinit-log.o service.o load-service.o proc-service.o baseproc-service.o run-child-proc.o

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
	$(CXX) $(SANITIZEOPTS) -o cptests cptests.o $(parent_test_objects) $(parent_objs) $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../dasynq -I../../../build/includes -c $< -o $@

$(parent_objs): %.o: ../../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../dasynq -I../../../build/includes -c $< -o $@

clean:
	rm -f *.o *.d cptests


# Experimental LLVM-libFuzzer based fuzzer. "make fuzz" to build; "fuzz corpus" to run (and store
# interesting test data in "corpus" directory).

fuzz_parent_test_objects = $(foreach obj,$(notdir $(parent_test_objects)),fuzz-$(obj))
fuzz_objects = $(foreach obj,$(parent_objs),fuzz-$(obj))

fuzz: fuzz.cc $(fuzz_parent_test_objects) $(fuzz_objects)
	clang++ -std=c++11 -g -O1 -Iincludes -I../../dasynq -fsanitize=fuzzer,address,undefined,leak fuzz.cc $(fuzz_parent_test_objects) $(fuzz_objects) -o fuzz

$(fuzz_parent_test_objects): fuzz-%.o: ../%.cc
	clang -O1 -fsanitize=address,undefined,fuzzer-no-link,leak -MMD -MP -I../includes -I../../dasynq -c $< -o $@

$(fuzz_objects): fuzz-%.o: ../../%.cc
	clang -O1 -fsanitize=address,undefined,fuzzer-no-link,leak -MMD -MP -Iincludes -I../../dasynq -c $< -o $@


-include $(objects:.o=.d)
-include $(parent_objects:.o=.d)
