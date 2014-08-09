ACLOCAL_AMFLAGS = -I m4

lib_LTLIBRARIES = libsylvan.la

libsylvan_la_CFLAGS = -g -O2 -Wall -Wextra -fno-strict-aliasing
libsylvan_la_CFLAGS += -I$(top_builddir)
libsylvan_la_CFLAGS += -I$(srcdir)/src
libsylvan_la_LIBADD = -lm

include_HEADERS = src/sylvan.h src/lace.h src/atomics.h

libsylvan_la_SOURCES = \
 src/barrier.c \
 src/barrier.h \
 src/sha2.c \
 src/sha2.h \
 src/sylvan.c \
 src/sylvan.h \
 src/llmsset.c \
 src/llmsset.h \
 src/cache.h \
 src/tls.h \
 src/atomics.h \
 src/avl.h \
 src/lace.c \
 src/lace.h \
 src/refs.h \
 src/hash24.h \
 src/hash16.h

if HAVE_NUMA
libsylvan_la_LIBADD += -lnuma
libsylvan_la_SOURCES += src/numa_tools.c src/numa_tools.h
libsylvan_la_CFLAGS += -DUSE_NUMA=1
endif
