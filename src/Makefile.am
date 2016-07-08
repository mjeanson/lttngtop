AM_CFLAGS = $(PACKAGE_CFLAGS) -I$(top_srcdir) -I$(top_srcdir)/lib -DBABELTRACE_HAVE_LIBUUID -DBABELTRACE_HAVE_FMEMOPEN -DBABELTRACE_HAVE_OPEN_MEMSTREAM

pkglib_LTLIBRARIES = libbabeltrace-lttngtop-live.la

libbabeltrace_lttngtop_live_la_SOURCES = \
				      network-live.c lttng-live-comm.c

bin_PROGRAMS = lttngtop

noinst_HEADERS = \
	lttngtoptypes.h \
	common.h \
	cputop.h \
	cursesdisplay.h \
	iostreamtop.h \
	mmap-live.h \
	network-live.h \
	lttng-live-comm.h \
	lttng-viewer-abi.h \
	lttngtop.h \
	lttng-session.h \
	$(top_builddir)/lib/babeltrace/align.h \
	$(top_builddir)/lib/babeltrace/babeltrace-internal.h \
	$(top_builddir)/lib/babeltrace/babeltrace.h \
	$(top_builddir)/lib/babeltrace/bitfield.h \
	$(top_builddir)/lib/babeltrace/clock-internal.h \
	$(top_builddir)/lib/babeltrace/clock-types.h \
	$(top_builddir)/lib/babeltrace/compat/limits.h \
	$(top_builddir)/lib/babeltrace/compat/memstream.h \
	$(top_builddir)/lib/babeltrace/compat/string.h \
	$(top_builddir)/lib/babeltrace/compat/utc.h \
	$(top_builddir)/lib/babeltrace/compat/uuid.h \
	$(top_builddir)/lib/babeltrace/compiler.h \
	$(top_builddir)/lib/babeltrace/context-internal.h \
	$(top_builddir)/lib/babeltrace/context.h \
	$(top_builddir)/lib/babeltrace/ctf-ir/metadata.h \
	$(top_builddir)/lib/babeltrace/ctf-text/types.h \
	$(top_builddir)/lib/babeltrace/ctf/callbacks-internal.h \
	$(top_builddir)/lib/babeltrace/ctf/callbacks.h \
	$(top_builddir)/lib/babeltrace/ctf/ctf-index.h \
	$(top_builddir)/lib/babeltrace/ctf/events-internal.h \
	$(top_builddir)/lib/babeltrace/ctf/events.h \
	$(top_builddir)/lib/babeltrace/ctf/iterator.h \
	$(top_builddir)/lib/babeltrace/ctf/metadata.h \
	$(top_builddir)/lib/babeltrace/ctf/types.h \
	$(top_builddir)/lib/babeltrace/endian.h \
	$(top_builddir)/lib/babeltrace/format-internal.h \
	$(top_builddir)/lib/babeltrace/format.h \
	$(top_builddir)/lib/babeltrace/iterator-internal.h \
	$(top_builddir)/lib/babeltrace/iterator.h \
	$(top_builddir)/lib/babeltrace/list.h \
	$(top_builddir)/lib/babeltrace/mmap-align.h \
	$(top_builddir)/lib/babeltrace/prio_heap.h \
	$(top_builddir)/lib/babeltrace/trace-collection.h \
	$(top_builddir)/lib/babeltrace/trace-handle-internal.h \
	$(top_builddir)/lib/babeltrace/trace-handle.h \
	$(top_builddir)/lib/babeltrace/types.h \
	$(top_builddir)/lib/babeltrace/bin-info.h \
	$(top_builddir)/lib/babeltrace/compat/dirent.h \
	$(top_builddir)/lib/babeltrace/compat/fcntl.h \
	$(top_builddir)/lib/babeltrace/compat/mman.h \
	$(top_builddir)/lib/babeltrace/compat/send.h \
	$(top_builddir)/lib/babeltrace/compat/stdio.h \
	$(top_builddir)/lib/babeltrace/compat/stdlib.h \
	$(top_builddir)/lib/babeltrace/crc32.h \
	$(top_builddir)/lib/babeltrace/debug-info.h \
	$(top_builddir)/lib/babeltrace/dwarf.h \
	$(top_builddir)/lib/babeltrace/trace-debug-info.h \
	$(top_builddir)/lib/babeltrace/utils.h



lttngtop_SOURCES = \
	lttngtop.c \
	common.c \
	cursesdisplay.c \
	cputop.c \
	iostreamtop.c \
	mmap-live.c \
	lttng-session.c

lttngtop_LDFLAGS = -Wl,--no-as-needed

lttngtop_LDADD = -lbabeltrace -lbabeltrace-ctf -llttng-ctl \
		 -lurcu -llttng-ust-ctl $(pkglib_LTLIBRARIES)
