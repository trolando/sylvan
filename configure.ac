AC_PREREQ([2.57])
AC_INIT([sylvan], [1.0])
AC_CONFIG_AUX_DIR([tools])
AM_INIT_AUTOMAKE([foreign no-define subdir-objects])
AC_CONFIG_MACRO_DIR([m4])

MAJOR_VERSION=`echo "$VERSION" | sed -e 's/^\([[0-9]]*\)\.[[0-9]]*.*$/\1/'`
MINOR_VERSION=`echo "$VERSION" | sed -e 's/^[[0-9]]*\.\([[0-9]]*\).*$/\1/'`
AC_DEFINE_UNQUOTED([MAJOR_VERSION], [$MAJOR_VERSION], [Sylvan's major version.])
AC_DEFINE_UNQUOTED([MINOR_VERSION], [$MINOR_VERSION], [Sylvan's minor version.])

AC_PROG_CC
AC_PROG_LIBTOOL

AC_ARG_WITH(numa, 
  AS_HELP_STRING([--with-numa], [Use NUMA allocators in Sylvan (default=no)]),
  [],
  [with_numa=no])

if test "x$with_numa" != xno; then
  AC_CHECK_HEADER([numa.h],
                  [numa=yes; AC_DEFINE([HAVE_NUMA_H], [1], [Define to 1 if you have <numa.h>])],
                  [AC_MSG_ERROR([Could not find numa.h!])])
fi

AM_CONDITIONAL([HAVE_NUMA_H], [test "$numa" = "yes"])

AC_CONFIG_HEADERS([src/config.h])

AC_CONFIG_FILES([Makefile])

AC_OUTPUT
