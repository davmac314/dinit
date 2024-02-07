include ../../mconfig

check-igr: igr-runner
	mkdir -p igr-output
	IGR_OUTPUT_BASE=$$PWD/igr-output ./igr-runner

igr-runner: igr-runner.cc igr.h
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) -I../../dasynq/include  $(LDFLAGS) $(LDFLAGS_EXTRA) igr-runner.cc -o igr-runner

clean:
	rm -f igr-runner
	rm -rf */output/
	rm -rf reload1/sd
	rm -rf reload2/sd
