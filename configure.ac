dnl Process this file with autoconf (via autoreconf -i) to produce configure.
dnl
dnl The package version is extracted from VERSION_STRING in src/version.h so
dnl that the version lives in exactly one place.  Because the extraction runs
dnl at autoreconf time (m4_esyscmd_s), re-run `autoreconf` after bumping
dnl src/version.h so configure picks up the new number.
AC_PREREQ([2.69])
AC_INIT([taskjuggler-lsp],
        [m4_esyscmd_s([sed -n 's/^#define VERSION_STRING *"\([^"]*\)".*/\1/p' src/version.h])],
        [https://github.com/devrintalen/taskjuggler-lsp/issues])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([1.14 -Wall foreign subdir-objects])

AC_PROG_CC
AC_PROG_RANLIB
AM_PROG_AR

dnl Flex and bison are invoked directly (bison -d must emit grammar.tab.h,
dnl which the flex output and most translation units #include), so require
dnl the real tools rather than generic lex/yacc.
AC_PATH_PROG([FLEX], [flex])
AS_IF([test -z "$FLEX"],
      [AC_MSG_ERROR([flex is required to build the lexer (src/lexer.l)])])
AC_PATH_PROG([BISON], [bison])
AS_IF([test -z "$BISON"],
      [AC_MSG_ERROR([bison is required to build the grammar (src/grammar.y)])])

dnl Python 3 drives the golden-file test harness (make check) and the
dnl benchmark tooling.  Missing python only disables `make check`.
AC_PATH_PROGS([PYTHON], [python3 python], [])
AS_IF([test -z "$PYTHON"],
      [AC_MSG_WARN([python3 not found; `make check` will not work])])

dnl Doxygen is optional and only needed for `make docs`.
AC_PATH_PROG([DOXYGEN], [doxygen])

AC_SEARCH_LIBS([pthread_create], [pthread], [],
               [AC_MSG_ERROR([a POSIX threads library is required])])

AC_CHECK_HEADER([yyjson.h], [],
                [AC_MSG_ERROR([yyjson.h not found; install yyjson (https://github.com/ibireme/yyjson)])])
dnl yyjson_read itself is a static-inline header wrapper, so probe an
dnl exported symbol instead.
AC_CHECK_LIB([yyjson], [yyjson_read_opts], [],
             [AC_MSG_ERROR([libyyjson not found; install yyjson (https://github.com/ibireme/yyjson)])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
