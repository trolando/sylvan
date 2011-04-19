ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir) -std=c99

bin_PROGRAMS = test

include_HEADERS = sylvan.h 

#check_PROGRAMS = bddtest
#bddtest_SOURCES = bddtest.cxx
#bddtest_LDADD = ./libbdd.la

test_SOURCES = main.c
test_LDADD = ../src/libsylvan.la
test_LDADD += -lpthread -lrt
