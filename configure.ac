AC_INIT([1984], [0.4.1], [], [1984])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign -Wall -Werror subdir-objects])
AC_CANONICAL_HOST

# C compiler — require C11
AC_PROG_CC
AX_CHECK_COMPILE_FLAG([-std=c11],
    [AM_CFLAGS="$AM_CFLAGS -std=c11"],
    [AC_MSG_ERROR([C11-capable compiler required])])
AC_SUBST([AM_CFLAGS])

# pkg-config
# On Haiku the SDL3 .pc file ships under a secondary-arch dir that
# pkg-config does not search by default. Prepend it so PKG_CHECK_MODULES
# below picks it up without the user setting PKG_CONFIG_PATH.
case "$host_os" in
  haiku*)
    for d in /boot/system/develop/lib/x86/pkgconfig \
             /boot/system/develop/lib/pkgconfig; do
      if test -d "$d"; then
        PKG_CONFIG_PATH="$d${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
      fi
    done
    export PKG_CONFIG_PATH
    ;;
esac
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
