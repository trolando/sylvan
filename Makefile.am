ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir)

AM_CFLAGS = -g -O3 -Wall
AM_CCASFLAGS = -g

include_HEADERS = src/sylvan.h

lib_LTLIBRARIES = libsylvan.la

libsylvan_la_SOURCES = \
 src/sylvan.c \
 src/llgcset.c \
 src/llsimplecache.c \
 src/sylvan.h 

libsylvan_la_SOURCES += src/wool.c

libsylvan_la_SOURCES += src/hash_mul.S
libsylvan_la_SOURCES += src/rehash_mul.S

libsylvan_la_LDFLAGS = -version-info 0:0:0
libsylvan_la_LIBADD = -lm 

if HAVE_NUMA_H
libsylvan_la_LIBADD += -lnuma
libsylvan_la_SROUCES += src/numa-tools.c
endif
