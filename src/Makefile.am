lib_LTLIBRARIES = libsylvan.la

libsylvan_la_CFLAGS = $(AM_CFLAGS) -fno-strict-aliasing -std=gnu11

libsylvan_la_SOURCES = \
    avl.h \
    lace.c \
    lace.h \
    llmsset.c \
    llmsset.h \
    sha2.c \
    sha2.h \
    sylvan.h \
    sylvan_bdd.h \
    sylvan_bdd.c \
    sylvan_cache.h \
    sylvan_cache.c \
    sylvan_config.h \
    sylvan_common.c \
    sylvan_common.h \
    sylvan_int.h \
    sylvan_ldd.h \
    sylvan_ldd.c \
    sylvan_mtbdd.h \
    sylvan_mtbdd.c \
    sylvan_mtbdd_int.h \
    sylvan_obj.hpp \
    sylvan_obj.cpp \
    sylvan_refs.h \
    sylvan_refs.c \
    sylvan_sl.h \
    sylvan_sl.c \
    sylvan_stats.h \
    sylvan_stats.c \
    tls.h

libsylvan_la_LIBADD = -lm

if HAVE_LIBHWLOC
libsylvan_la_LIBADD += -lhwloc
libsylvan_la_CFLAGS += -DUSE_HWLOC=1
endif
