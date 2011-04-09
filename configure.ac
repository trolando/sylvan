AC_PREREQ([2.57])
AC_INIT([sylvan], [1.0])
AC_CONFIG_AUX_DIR([tools])
AM_INIT_AUTOMAKE([foreign nostdinc no-define 1.7.3])
AC_CONFIG_MACRO_DIR([m4])

MAJOR_VERSION=`echo "$VERSION" | sed -e 's/^\([[0-9]]*\)\.[[0-9]]*.*$/\1/'`
MINOR_VERSION=`echo "$VERSION" | sed -e 's/^[[0-9]]*\.\([[0-9]]*\).*$/\1/'`
AC_DEFINE_UNQUOTED([MAJOR_VERSION], [$MAJOR_VERSION], [Sylvan's major version.])
AC_DEFINE_UNQUOTED([MINOR_VERSION], [$MINOR_VERSION], [Sylvan's minor version.])

AC_PROG_CC

AM_PROG_LEX
AC_PROG_YACC

AC_PROG_CXX

AC_PROG_LIBTOOL

AC_CONFIG_HEADERS([config.h])

AC_CONFIG_FILES([
  Makefile
  src/Makefile
])

AC_OUTPUT
