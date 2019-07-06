include ../../mconfig

check-igr: igr-runner
	./igr-runner

igr-runner: igr-runner.cc
	$(CXX) $(CXXOPTS) igr-runner.cc -o igr-runner

clean:
	rm -f igr-runner basic/basic-ran environ/env-record ps-environ/env-record

