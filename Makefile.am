
## Process this file with automake to produce Makefile.in
## Makefile.am, (c)1999 Tom Wheeley <tw104@york.ac.uk>
ACLOCAL_AMFLAGS             = -I m4

## we need version 1.5 of automake for DIST_SUBDIRS to work and dist-bzip2.
AUTOMAKE_OPTIONS = 1.5 check-news dist-bzip2

EXTRA_DIST =  setup.sh setup-driver.sh setup-functions.sh setup.data data2setup.sh README TODO
EXTRA_DIST += autogen.sh

dist_noinst_DATA = drivers/lirc.h  # Temporary fix after removing drivers/

## daemons/ and tools/ should appear before doc/ for help2man to work.
if SYSTEMD_INSTALL
    SYSTEMD_DIR = systemd
endif

SUBDIRS = daemons tools doc $(SYSTEMD_DIR)

if INSTALL_ETC
lirc_confdir        = $(sysconfdir)/lirc
dist_lirc_conf_DATA = lircd.conf lirc_options.conf lircmd.conf
endif

## From automake documentation:
## Note that EXTRA_DIST can only handle files in the current
## directory; files in other directories will cause make dist runtime
## failures.

## ----------------------------------------------------------------------
## flags to pass to ./configure when calling "make distcheck" and "make
## distcheck-light". Try to check as many features as possible! Also
## need to give enable-sandboxed, so that staged install does not fail.

## !!! BIG WARNING: this needs to be tweaked a bit more !!! ##

DISTCHECK_FLAGS = --with-driver=userspace --enable-sandboxed --sysconfdir=/tmp/etc
DISTCHECK_CONFIGURE_FLAGS = ${DISTCHECK_FLAGS}
DISTCHECK_LIGHT_FLAGS = --with-driver=none --enable-sandboxed

distcheck-light:
	$(MAKE) $(AM_MAKEFLAGS) DISTCHECK_FLAGS="$(DISTCHECK_LIGHT_FLAGS)" distcheck

m4dir                       = m4
# dist_m4_DATA                = m4/ac-path-kernel-source.m4

# ----------------------------------------------------------------------

setup-driver.sh: data2setup.sh setup.data
	./data2setup.sh > setup-driver.sh

dist-hook:
	(cd $(srcdir)/contrib/hal/ && ./gen-hal-fdi.pl)
	for f in contrib remotes; do \
		cp -pr $(srcdir)/$$f $(distdir)/$$f; done
	git log >$(distdir)/ChangeLog || :

DISTCLEANFILES = configure.sh .setup.config

CLEANFILES = *~ remotes/*~ contrib/*~

lirc.org: .phony
	tar czf lirc.org.tar.gz lirc.org --dereference

.phony:

