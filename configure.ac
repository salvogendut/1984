AC_INIT([1984], [0.4.1], [], [1984])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([foreign -Wall -Werror subdir-objects])

# C compiler — require C11
AC_PROG_CC
AX_CHECK_COMPILE_FLAG([-std=c11],
    [AM_CFLAGS="$AM_CFLAGS -std=c11"],
    [AC_MSG_ERROR([C11-capable compiler required])])
AC_SUBST([AM_CFLAGS])

# pkg-config
PKG_PROG_PKG_CONFIG

# Required libraries
PKG_CHECK_MODULES([SDL3], [sdl3], [], [AC_MSG_ERROR([SDL3 not found])])
AC_CHECK_LIB([m], [sqrt], [], [AC_MSG_ERROR([libm not found])])

# Winsock on Windows hosts (sockets live in ws2_32, not libc).
# Also force the console subsystem: SDL3's pkg-config adds -mwindows, which
# detaches the process from the console and silently swallows stderr/stdout.
# -mconsole (placed last on the link line, see Makefile.am) overrides it so
# errors and --trace-io output are visible when run from cmd.exe.
case $host_os in
  mingw*|cygwin*|msys*) WIN_LIBS="-lws2_32"; WIN_SUBSYSTEM="-mconsole" ;;
  *)                    WIN_LIBS="";         WIN_SUBSYSTEM="" ;;
esac
AC_SUBST([WIN_LIBS])
AC_SUBST([WIN_SUBSYSTEM])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
