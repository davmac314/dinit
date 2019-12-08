include ../../mconfig

check-igr: igr-runner
	./igr-runner

igr-runner: igr-runner.cc
	$(CXX) $(CXXOPTS) igr-runner.cc -o igr-runner

clean:
	rm -f igr-runner basic/basic-ran environ/env-record ps-environ/env-record chain-to/recorded-output
	rm -f restart/basic-ran
	rm -f check-basic/output.txt check-cycle/output.txt
	rm -rf reload1/sd
	rm -rf reload2/sd
