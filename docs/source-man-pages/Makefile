include ../../build/version.conf

all: generate

generate: dinit.8 dinitctl.8 dinitcheck.8 dinit-monitor.8 dinit-service.5 shutdown.8

clean:
	rm -f dinit.8 dinitctl.8 dinitcheck.8 dinit-monitor.8 dinit-service.5 shutdown.8

html: generate
	../generate-html.sh

dinit.8: dinit.8.m4
	m4 -DVERSION=$(VERSION) -DMONTH=$(MONTH) -DYEAR=$(YEAR) dinit.8.m4 > dinit.8

dinitctl.8: dinitctl.8.m4
	m4 -DVERSION=$(VERSION) -DMONTH=$(MONTH) -DYEAR=$(YEAR) dinitctl.8.m4 > dinitctl.8

dinitcheck.8: dinitcheck.8.m4
	m4 -DVERSION=$(VERSION) -DMONTH=$(MONTH) -DYEAR=$(YEAR) dinitcheck.8.m4 > dinitcheck.8

dinit-monitor.8: dinit-monitor.8.m4
	m4 -DVERSION=$(VERSION) -DMONTH=$(MONTH) -DYEAR=$(YEAR) dinit-monitor.8.m4 > dinit-monitor.8

dinit-service.5: dinit-service.5.m4
	m4 -DVERSION=$(VERSION) -DMONTH=$(MONTH) -DYEAR=$(YEAR) dinit-service.5.m4 > dinit-service.5

shutdown.8: shutdown.8.m4
	m4 -DVERSION=$(VERSION) -DMONTH=$(MONTH) -DYEAR=$(YEAR) shutdown.8.m4 > shutdown.8
