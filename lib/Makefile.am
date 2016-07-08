AM_CFLAGS = $(PACKAGE_CFLAGS) -I$(top_srcdir) -I$(top_srcdir)/lib -DBABELTRACE_HAVE_LIBUUID -DBABELTRACE_HAVE_FMEMOPEN -DBABELTRACE_HAVE_OPEN_MEMSTREAM

lib_LTLIBRARIES = libmmappacketseek.la

libmmappacketseek_la_SOURCES = mmap-packet-seek.c

libmmappacketseek_la_LDFLAGS = \
	-Wl,--no-as-needed \
	-lbabeltrace_types
