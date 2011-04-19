ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_builddir) -I$(top_builddir)/src -std=c99

bin_PROGRAMS = test

#check_PROGRAMS = bddtest
#bddtest_SOURCES = bddtest.cxx
#bddtest_LDADD = ./libbdd.la

test_SOURCES = main.c
test_LDADD = ../src/libsylvan.la
test_LDADD += -lpthread -lrt
