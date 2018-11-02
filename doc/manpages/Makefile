include ../../mconfig

install:
	mkdir -p "$(DESTDIR)$(MANDIR)/man8"
	install -m644 dinit.8 dinitctl.8 "$(DESTDIR)$(MANDIR)/man8"
	mkdir -p "$(DESTDIR)$(MANDIR)/man5"
	install -m644 dinit-service.5 "$(DESTDIR)$(MANDIR)/man5"
ifeq ($(BUILD_SHUTDOWN),yes)
	install -m644 shutdown.8 "$(DESTDIR)$(MANDIR)/man8"
	ln "$(DESTDIR)$(MANDIR)/man8/shutdown.8" "$(DESTDIR)$(MANDIR)/man8/halt.8" 
	ln "$(DESTDIR)$(MANDIR)/man8/shutdown.8" "$(DESTDIR)$(MANDIR)/man8/reboot.8" 
endif
