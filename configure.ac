AC_INIT([stomp], [0.1], [evgeni_dobrev@developer.bg],[],[https://github.com/evgenido/stomp])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([-Wall foreign 1.11 subdir-objects])
AM_SILENT_RULES([yes])
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
LT_INIT
AC_PROG_CC
PKG_CHECK_MODULES(CHECK,[check], [HAVE_CHECK=yes], [HAVE_CHECK=no])
AM_CONDITIONAL(HAVE_CHECK, test x$HAVE_CHECK = xyes)

AC_SUBST([STOMP_SO_VERSION], [0:0:0])

AC_CONFIG_FILES([Makefile src/Makefile tests/Makefile examples/Makefile stomp.pc])
AC_OUTPUT
