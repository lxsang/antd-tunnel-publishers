AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -W  -Wall -g -std=c99

# bin
bin_PROGRAMS = runner
# source files
runner_SOURCES = runner.c

#sysconf_DATA = runner.ini
install-data-local:
	- cp runner.ini $(DESTDIR)/$(prefix)/
	- cp runnerd $(DESTDIR)/$(prefix)/bin
	- [ -d $(DESTDIR)/etc/systemd/system/ ] && cp antd-tunnel-publisher.service $(DESTDIR)/etc/systemd/system/

EXTRA_DIST = runner.ini runnerd tunnel.h antd-tunnel-publisher.service log.h

SUBDIRS = . vterm wfifo syslog broadcast

if ENABLE_CAM
    SUBDIRS += v4l2cam
endif
