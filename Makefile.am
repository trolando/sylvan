ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir)

include_HEADERS = src/sylvan.h

lib_LTLIBRARIES = libsylvan.la

libsylvan_la_SOURCES = \
 src/sylvan.c \
 src/llgcset.c \
 src/llvector.c \
 src/runtime.c \
 src/fast_hash.c \
 src/sylvan.h \
 src/llvector.h \
 src/runtime.h

libsylvan_la_LDFLAGS = -version-info 0:0:0
libsylvan_la_LIBADD = -lm
