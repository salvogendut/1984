AC_INIT([1984], [0.4.14], [], [1984])
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

# Cairo is optional: it backs the host-side PDF printer sink. Without
# it, the printer compiles to a no-op (port 0xEFxx writes are still
# decoded for status/LED purposes but nothing is captured to disk).
# Pass --without-cairo to skip detection.
AC_ARG_WITH([cairo],
  [AS_HELP_STRING([--without-cairo],
                  [build without the PDF printer backend])],
  [], [with_cairo=yes])
if test "x$with_cairo" != "xno"; then
  PKG_CHECK_MODULES([CAIRO], [cairo], [],
    [AC_MSG_WARN([cairo not found — building without PDF printer])
     with_cairo=no])
fi
if test "x$with_cairo" = "xyes"; then
  AC_DEFINE([HAVE_CAIRO], [1], [cairo available for PDF printer])
fi
AC_MSG_NOTICE([PDF printer backend: $with_cairo])

# Platform-specific socket linkage and Windows extras.
#   - Windows (MinGW): sockets live in ws2_32. The build picks up SDL3's
#     -mwindows (no console window). icons/1984.rc is compiled with
#     windres into a .o and linked in so the .exe gets its icon.
#   - Haiku: sockets live in libnetwork.
#   - Solaris/illumos: sockets live in libsocket.
#   - Linux / *BSD / macOS: sockets are in libc; nothing extra needed.
case $host_os in
  mingw*|cygwin*|msys*)
    WIN_LIBS="-lws2_32"
    AC_CHECK_TOOL([WINDRES], [windres], [windres])
    WIN_RC_OBJ="icons/1984-rc.o"
    ;;
  *)
    WIN_LIBS=""
    WIN_RC_OBJ=""
    AC_SEARCH_LIBS([socket],     [network socket])
    AC_SEARCH_LIBS([getsockopt], [network socket])
    ;;
esac
AC_SUBST([WIN_LIBS])
AC_SUBST([WIN_RC_OBJ])
AC_SUBST([WINDRES])

# ffmpeg — optional. Enables WebM/VP9 video capture from the overlay's
# "Capture video" menu. The F6-triggered .gif capture works without it.
# Detected at configure time so the overlay can gate the menu row; the
# path is hardcoded into the binary as FFMPEG_PATH for popen().
AC_PATH_PROG([FFMPEG], [ffmpeg], [])
AS_IF([test -n "$FFMPEG"],
      [AC_DEFINE_UNQUOTED([FFMPEG_PATH], ["$FFMPEG"],
           [Path to ffmpeg binary for WebM video capture])
       AC_DEFINE([HAVE_FFMPEG], [1], [ffmpeg available for WebM capture])
       AC_MSG_NOTICE([video capture: WebM enabled via $FFMPEG])],
      [AC_MSG_NOTICE([video capture: WebM disabled (no ffmpeg found)])])

# Short git commit shown in the Advanced overlay alongside PACKAGE_VERSION.
# Resolved at configure time so non-git source trees (dist tarballs, flatpak
# tarball builds) still get a sensible "unknown" fallback.
PROG_GIT_COMMIT=`git -C "$srcdir" rev-parse --short HEAD 2>/dev/null || echo unknown`
AC_MSG_NOTICE([git commit: $PROG_GIT_COMMIT])
AC_SUBST([PROG_GIT_COMMIT])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
