ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir) -std=c99

include_HEADERS = sylvan.h 

lib_LTLIBRARIES = libsylvan.la
libsylvan_la_SOURCES = \
 sylvan.c \
 llsched.c \
 llset.c \
 llvector.c \
 runtime.c \
 fast_hash.c \
 sylvan.h \
 llsched.h \
 llset.h \
 llvector.h \
 runtime.h

# See the `Updating version info' node of the Libtool manual before
# changing this.
libsylvan_la_LDFLAGS = -version-info 0:0:0
libsylvan_la_LIBADD = -lm

#check_PROGRAMS = bddtest
#bddtest_SOURCES = bddtest.cxx
#bddtest_LDADD = ./libbdd.la
