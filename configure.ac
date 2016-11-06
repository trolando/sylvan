AC_PREREQ([2.60])
AC_INIT([sylvan], [1.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([tools])
AM_INIT_AUTOMAKE([foreign])

AC_PROG_CC
AX_CHECK_COMPILE_FLAG([-std=c11],,[AC_MSG_FAILURE([no acceptable C11 compiler found.])])
AC_PROG_CXX
LT_INIT

PKG_PROG_PKG_CONFIG
m4_pattern_forbid([^PKG_[A-Z_]+$], [missing some pkg-config macros (pkg-config package)])

PKG_CHECK_MODULES([HWLOC], [hwloc], [hwloc=yes],
    [AC_MSG_WARN(No suitable hwloc installation found)])
AM_CONDITIONAL([HAVE_LIBHWLOC], [test "$hwloc" = "yes"])

AC_CANONICAL_HOST
AM_CONDITIONAL([DARWIN], [case $host_os in darwin*) true;; *) false;; esac])
# test x$(uname) == "xDarwin"])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
