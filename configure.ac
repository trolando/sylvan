AC_PREREQ([2.57])
AC_INIT([sylvan], [1.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([tools])
AM_INIT_AUTOMAKE([foreign no-define subdir-objects])

AC_PROG_CC
AC_PROG_LIBTOOL
AC_SUBST(LIBTOOL_DEPS)
AC_C_CONST

AC_ARG_WITH(numa, 
  AS_HELP_STRING([--with-numa], [Use NUMA allocators in Sylvan (default=no)]),
  [case $with_numa in
     no)
       ;;
     yes)
       AC_CHECK_HEADERS([numa.h], AC_CHECK_LIB([numa], [numa_available], numa=yes))
       ;;
   esac
  ],
  [numa=no])

AM_CONDITIONAL([HAVE_NUMA], [test "$numa" = "yes"])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
