/* compat_win.h — cross-platform shims so the emulator builds with MinGW.
 *
 * On POSIX this just pulls in the usual networking / filesystem headers and
 * maps the helper names below onto their native calls. On Windows it pulls in
 * Winsock and supplies the handful of POSIX facilities MinGW lacks
 * (fnmatch, statvfs free-space, the BSD socket idioms, and mkdir's mode arg).
 *
 * Include this *before* anything that might drag in <windows.h> (so Winsock2
 * wins over the legacy winsock1 declarations).
 */
#ifndef COMPAT_WIN_H
#define COMPAT_WIN_H

/* ===========================================================================
 * Windows / MinGW
 * ========================================================================= */
#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   /* Vista+: WSAPoll, struct pollfd, getaddrinfo */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>   /* socklen_t, getaddrinfo, IPPROTO_TCP */
#include <windows.h>
#include <direct.h>     /* _mkdir */
#include <io.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>

/* --- mkdir takes no mode argument on Windows --- */
#define mkdir(path, mode)   _mkdir(path)

/* recv/send flags Winsock lacks. Sockets here are already non-blocking, and
 * there is no SIGPIPE on Windows, so 0 is the correct behaviour. */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* --- BSD socket idioms --- */
#define sock_close(fd)      closesocket(fd)

static inline int sock_set_nonblocking(int fd) {
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
}

/* connect() on a non-blocking socket "in progress" */
#define sock_in_progress()  (WSAGetLastError() == WSAEWOULDBLOCK)
/* recv()/send() would block (no data / buffer full) */
#define sock_would_block()  (WSAGetLastError() == WSAEWOULDBLOCK)

/* bytes available to read, into an int */
static inline int sock_fionread(int fd, int *avail) {
    u_long n = 0;
    int r = ioctlsocket((SOCKET)fd, FIONREAD, &n);
    *avail = (int)n;
    return r;
}

/* poll() -> WSAPoll(); struct pollfd / POLL* come from winsock2.h */
#define poll(fds, n, timeout)  WSAPoll(fds, n, timeout)

/* --- Winsock startup (call once at program start) --- */
static inline int net_compat_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

/* --- fnmatch() shim (MinGW has none) -------------------------------------
 * Supports '*' and '?' plus FNM_CASEFOLD; FNM_NOESCAPE is implied (backslash
 * is a literal, which is what we want on FAT names anyway). Enough for the
 * directory filters M4ROM uses. */
#define FNM_NOMATCH   1
#define FNM_NOESCAPE  0x01
#define FNM_CASEFOLD  0x10

static inline int compat_fnmatch(const char *pat, const char *str, int flags) {
    int fold = (flags & FNM_CASEFOLD) != 0;
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 0;                 /* trailing '*' matches rest */
            for (; *str; str++)
                if (compat_fnmatch(pat, str, flags) == 0) return 0;
            return FNM_NOMATCH;
        } else if (*pat == '?') {
            if (!*str) return FNM_NOMATCH;
            pat++; str++;
        } else {
            char a = *pat, b = *str;
            if (fold) { a = (char)tolower((unsigned char)a);
                        b = (char)tolower((unsigned char)b); }
            if (a != b) return FNM_NOMATCH;
            pat++; str++;
        }
    }
    return *str ? FNM_NOMATCH : 0;
}
#define fnmatch(pat, str, flags)  compat_fnmatch(pat, str, flags)

/* --- free bytes on the volume containing `path` --- */
static inline unsigned long long compat_fs_free_bytes(const char *path) {
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExA(path, &avail, NULL, NULL))
        return (unsigned long long)avail.QuadPart;
    return 0;
}

/* ===========================================================================
 * POSIX
 * ========================================================================= */
#else  /* !_WIN32 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <errno.h>

#define sock_close(fd)      close(fd)

static inline int sock_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#define sock_in_progress()  (errno == EINPROGRESS)
#define sock_would_block()  (errno == EAGAIN || errno == EWOULDBLOCK)

static inline int sock_fionread(int fd, int *avail) {
    return ioctl(fd, FIONREAD, avail);
}

static inline int net_compat_init(void) { return 0; }

static inline unsigned long long compat_fs_free_bytes(const char *path) {
    struct statvfs sv;
    if (statvfs(path, &sv) == 0)
        return (unsigned long long)sv.f_bavail * sv.f_bsize;
    return 0;
}

#endif  /* _WIN32 */

#endif  /* COMPAT_WIN_H */
