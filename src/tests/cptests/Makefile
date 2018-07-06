-include ../../../mconfig

objects = cptests.o
parent_test_objects = ../test-bpsys.o ../test-dinit.o
parent_objs = control.o dinit-log.o service.o

check: build-tests
	./cptests

build-tests: prepare-incdir cptests

# Create an "includes" directory populated with a combination of real and mock headers:
prepare-incdir:
	mkdir -p includes
	rm -rf includes/*.h
	cd includes; ln -f ../../../includes/*.h .
	cd includes; ln -f ../../test-includes/dinit.h .
	cd includes; ln -f ../../test-includes/baseproc-sys.h .

cptests: cptests.o $(parent_objs)
	$(CXX) $(SANITIZEOPTS) -o cptests cptests.o $(parent_test_objects) $(parent_objs) $(LDFLAGS)

$(objects): %.o: %.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../dasynq -c $< -o $@

$(parent_objs): %.o: ../../%.cc
	$(CXX) $(CXXOPTS) $(SANITIZEOPTS) -MMD -MP -Iincludes -I../../dasynq -c $< -o $@

clean:
	rm -f *.o *.d cptests

-include $(objects:.o=.d)
-include $(parent_objects:.o=.d)
