include ../../mconfig

check-igr: igr-runner
	IGR_FUNCTIONS="../igr_functions" EXEC_PREFIX="../../" MAKE=1 ./igr-runner

igr-runner: igr-runner.cc
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) $(LDFLAGS) $(LDFLAGS_EXTRA) igr-runner.cc -o igr-runner

clean:
	rm -rf */output/
	rm -f igr-runner basic/basic-ran environ/env-record environ2/env-record ps-environ/env-record chain-to/recorded-output var-subst/args-record
	rm -f restart/basic-ran
	rm -f check-basic/output.txt check-lint/output.txt check-cycle/output.txt no-command-error/dinit-run.log
	rm -rf reload1/sd
	rm -rf reload2/sd
	rm -rf pseudo-cycle/output/*
	rm -rf before-after/output/*
