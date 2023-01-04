include ../../mconfig

check-igr: igr-runner
	DINIT_EXEC=../../dinit DINITCTL_EXEC=../../dinitctl DINITCHECK_EXEC=../../dinitcheck DINITMONITOR_EXEC=../../dinit-monitor ./igr-runner

igr-runner: igr-runner.cc
	$(CXX) $(CPPFLAGS) $(CXXOPTS) $(CXXFLAGS) igr-runner.cc -o igr-runner

clean:
	rm -f igr-runner basic/basic-ran environ/env-record ps-environ/env-record chain-to/recorded-output var-subst/args-record
	rm -f restart/basic-ran
	rm -f check-basic/output.txt check-lint/output.txt check-cycle/output.txt no-command-error/dinit-run.log
	rm -rf reload1/sd
	rm -rf reload2/sd
