include ../../mconfig

check-igr: igr-runner
	mkdir -p igr-output
	IGR_OUTPUT_BASE=$$PWD/igr-output ./igr-runner

igr-runner: igr-runner.cc igr.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXFLAGS_EXTRA) -I../../dasynq/include -I../../build/includes $(LDFLAGS) $(LDFLAGS_EXTRA) igr-runner.cc -o igr-runner

clean:
	rm -f igr-runner
	rm -rf */output/
	rm -rf reload1/sd
	rm -rf reload2/sd
