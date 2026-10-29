/* Mediathek - libc symbols that FFmpeg's static libraries reference and the app's runtime lacks.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The app links against a small libc module (runtime/libc.prx). PacBrew's
 * FFmpeg drags in OpenSSL (for https), iconv (for text subtitles) and a few
 * time functions; their object files are linked even though a local movie never
 * runs them. Each function here is either a real, small implementation
 * (the time functions) or a stub for a path that is not taken. A stub that
 * were taken would fail loudly (-1/ENOSYS, or a trap), never quietly.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "platform/ps5/dns.h"

/* The console's time zone is not known to the app: local time is UTC. */
struct tm *gmtime_r(const time_t *timer, struct tm *out)
{
    int64_t seconds = (int64_t)*timer;
    int64_t days = seconds / 86400;
    int64_t rest = seconds % 86400;
    if (rest < 0)
    {
        rest += 86400;
        days--;
    }
    out->tm_hour = (int)(rest / 3600);
    out->tm_min = (int)(rest % 3600 / 60);
    out->tm_sec = (int)(rest % 60);
    int weekday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    out->tm_wday = weekday < 0 ? weekday + 7 : weekday;

    /* Civil date from a day count (H. Hinnant's algorithm). */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t day_of_era = z - era * 146097;
    int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int64_t year = year_of_era + era * 400;
    int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int64_t mp = (5 * day_of_year + 2) / 153;
    int64_t day = day_of_year - (153 * mp + 2) / 5 + 1;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2)
        year++;

    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0;
    out->tm_year = (int)(year - 1900);
    out->tm_mon = (int)(month - 1);
    out->tm_mday = (int)day;
    out->tm_yday = before[month - 1] + (int)day - 1 + (month > 2 ? leap : 0);
    out->tm_isdst = 0;
    return out;
}

struct tm *localtime_r(const time_t *timer, struct tm *out)
{
    return gmtime_r(timer, out);
}

/* libiconv asks for the locale's character set; the app only ever has UTF-8. */
char *nl_langinfo(int item)
{
    (void)item;
    return (char *)"UTF-8";
}
int ___mb_cur_max = 1;

/* OpenSSL's async fibres and datagram sockets, and a symbol lookup: not used. */
int _setjmp(void *env)
{
    (void)env;
    return 0;
}
void _longjmp(void *env, int value)
{
    (void)env;
    (void)value;
    __builtin_trap();
}
ssize_t sendmmsg(int fd, struct mmsghdr *messages, size_t count, int flags)
{
    (void)fd;
    (void)messages;
    (void)count;
    (void)flags;
    errno = ENOSYS;
    return -1;
}
ssize_t recvmmsg(int fd, struct mmsghdr *messages, size_t count, int flags,
                 const struct timespec *timeout)
{
    (void)fd;
    (void)messages;
    (void)count;
    (void)flags;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}
int dladdr(const void *address, void *info)
{
    (void)address;
    (void)info;
    return 0;
}

/* FFmpeg's network protocols and OpenSSL resolve host names and seed from
 * arc4random_buf. The SDK would supply these from libScePosixForWebKit.prx, a
 * library of the browser process that a title must not depend on: one
 * unresolvable import and the console refuses to start the app. The lookup is
 * the small DNS client in dns.c; the random bytes come from the kernel. */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    return mt_getaddrinfo(node, service, hints, result);
}
void freeaddrinfo(struct addrinfo *info)
{
    mt_freeaddrinfo(info);
}
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, size_t host_size,
                char *service, size_t service_size, int flags)
{
    (void)address;
    (void)length;
    (void)host;
    (void)host_size;
    (void)service;
    (void)service_size;
    (void)flags;
    return EAI_FAIL;
}
const char *gai_strerror(int code)
{
    (void)code;
    return "name resolution failed";
}
struct hostent *gethostbyname(const char *name)
{
    (void)name;
    return NULL;
}

/* Random bytes for TLS keys: the kernel's generator (sysctl kern.arandom), else
 * /dev/urandom. mt_random_available() tells the caller whether either works, so
 * a secure connection is refused rather than made with a weak seed. */
static int kernel_random(void *buffer, size_t size)
{
    unsigned char *out = (unsigned char *)buffer;
    while (size > 0)
    {
        int mib[2] = {CTL_KERN, KERN_ARND};
        size_t got = size > 256 ? 256 : size;
        if (sysctl(mib, 2, out, &got, NULL, 0) != 0 || got == 0)
            break;
        out += got;
        size -= got;
    }
    if (size == 0)
        return 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    while (size > 0)
    {
        ssize_t got = read(fd, out, size);
        if (got <= 0)
            break;
        out += got;
        size -= (size_t)got;
    }
    close(fd);
    return size == 0 ? 0 : -1;
}

int mt_random_available(void)
{
    unsigned char probe[16];
    return kernel_random(probe, sizeof probe) == 0;
}

void arc4random_buf(void *buffer, size_t size)
{
    if (kernel_random(buffer, size) != 0)
        __builtin_trap(); /* no entropy: stop, never hand out guessable bytes */
}
