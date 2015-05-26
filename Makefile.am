ACLOCAL_AMFLAGS = -I m4
AM_CFLAGS = -I$(top_srcdir)/src

lib_LTLIBRARIES = libsylvan.la

libsylvan_la_CFLAGS = $(AM_CFLAGS) -g -O2 -Wall -Wextra -fno-strict-aliasing
libsylvan_la_LIBADD = -lm

include_HEADERS = src/sylvan.h src/lace.h src/atomics.h

libsylvan_la_SOURCES = \
    src/barrier.c \
    src/barrier.h \
    src/sha2.c \
    src/sha2.h \
    src/sylvan.h \
    src/sylvan_config.h \
    src/sylvan_bdd.h \
    src/sylvan_bdd.c \
    src/sylvan_ldd.h \
    src/sylvan_ldd.c \
    src/llmsset.c \
    src/llmsset.h \
    src/sylvan_common.c \
    src/sylvan_common.h \
    src/cache.h \
    src/tls.h \
    src/atomics.h \
    src/avl.h \
    src/lace.c \
    src/lace.h \
    src/refs.h \
    src/refs.c \
    src/getrss.c \
    src/getrss.h \
    src/hash16.h

if HAVE_LIBHWLOC
libsylvan_la_LIBADD += -lhwloc
libsylvan_la_CFLAGS += -DUSE_HWLOC=1
endif

noinst_PROGRAMS = sylvan_test

sylvan_test_SOURCES = test/main.c
sylvan_test_LDADD = libsylvan.la -lpthread
