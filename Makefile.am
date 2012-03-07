ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir)

include_HEADERS = src/sylvan.h

lib_LTLIBRARIES = libsylvan.la

libsylvan_la_SOURCES = \
 src/sylvan.c \
 src/llgcset.c \
 src/rng_hash_128.s \
 src/hash_mul.s \
 src/rehash_mul.s \
 src/llcache.c \
 src/llsimplecache.c \
 src/sylvan.h 

libsylvan_la_LDFLAGS = -version-info 0:0:0
libsylvan_la_LIBADD = -lm 

if HAVE_NUMA_H
libsylvan_la_LIBADD += -lnuma
endif
