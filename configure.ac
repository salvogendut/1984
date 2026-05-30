AC_INIT([1984], [0.4.1], [], [1984])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
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

# Sockets live in libnetwork on Haiku, libsocket on Solaris/illumos,
# and libc on Linux/BSD/macOS.
AC_SEARCH_LIBS([socket], [network socket])
AC_SEARCH_LIBS([getsockopt], [network socket])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
