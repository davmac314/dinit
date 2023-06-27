include ../../mconfig

check-igr: igr-runner
	./igr-runner

igr-runner: igr-runner.cc
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) $(LDFLAGS) $(LDFLAGS_EXTRA) igr-runner.cc -o igr-runner

clean:
	rm -f igr-runner
	rm -rf */output/
	rm -rf reload1/sd
	rm -rf reload2/sd
