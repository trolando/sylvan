ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS += -I$(srcdir)/src

AM_CFLAGS = -g -O2 -Wall -Wextra

include_HEADERS = src/sylvan.h

lib_LTLIBRARIES = libsylvan.la

libsylvan_la_SOURCES = \
 src/barrier.c \
 src/barrier.h \
 src/sylvan.c \
 src/sylvan.h \
 src/llmsset.c \
 src/llmsset.h \
 src/llci.h \
 src/tls.h \
 src/atomics.h \
 src/avl.h \
 src/lace.c \
 src/lace.h \
 src/ticketlock.h \
 src/murmur.c \
 src/murmur.h

libsylvan_la_LDFLAGS = -version-info 0:0:0
libsylvan_la_LIBADD = -lm

if HAVE_NUMA_H
libsylvan_la_LIBADD += -lnuma
libsylvan_la_SOURCES += src/numa_tools.c src/numa_tools.h
AM_CFLAGS += -DUSE_NUMA=1
endif

EXTRA_DIST = $(libsylvan_la_SOURCES)
