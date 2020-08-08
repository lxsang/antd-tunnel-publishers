AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -W  -Wall -g -std=c99

# bin
bin_PROGRAMS = runner
# source files
runner_SOURCES = runner.c

#sysconf_DATA = runner.ini
install-data-local:
	- cp runner.ini $(prefix)/
	- cp runnerd $(prefix)/bin
	- [ -d /etc/systemd/system/ ] && cp antd-tunnel-publisher.service /etc/systemd/system/

EXTRA_DIST = runner.ini runnerd tunnel.h antd-tunnel-publisher.service log.h

SUBDIRS = . vterm