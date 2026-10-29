/* Mediathek - A small DNS client: the console app has no resolver of its own.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <netdb.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* The nameserver to ask first, as dotted digits ("" or NULL = only the built-in ones). */
    void mt_dns_set_server(const char *dotted);

    /* Looks up IPv4 addresses (network byte order) over UDP port 53. Returns how many
     * were found, 0 when none. */
    int mt_dns_lookup(const char *host, uint32_t *addresses, int capacity);

    /* True when the kernel's random generator can be read (libc_gaps.c). */
    int mt_random_available(void);

    /* getaddrinfo / freeaddrinfo for IPv4 TCP and UDP, built on the above. */
    int mt_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result);
    void mt_freeaddrinfo(struct addrinfo *info);

#ifdef __cplusplus
}
#endif
