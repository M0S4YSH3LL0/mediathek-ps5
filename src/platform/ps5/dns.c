/* Mediathek - A small DNS client: the console app has no resolver of its own.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The system resolver lives in a library a title should not depend on (see
 * libc_gaps.c), so names are looked up here: one A-record question over UDP to
 * the configured nameserver, then to well-known public ones. Only IPv4 answers
 * are read. Good enough to reach a handful of web hosts; not a general resolver.
 */

#include "platform/ps5/dns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DNS_TIMEOUT_SECONDS 2
#define DNS_PACKET 512

static char g_server[48];
static const char *const g_fallback[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9", NULL};

void mt_dns_set_server(const char *dotted)
{
    g_server[0] = '\0';
    size_t length = dotted != NULL ? strlen(dotted) : 0;
    if (dotted != NULL && length < sizeof g_server)
        memcpy(g_server, dotted, length + 1);
}

static size_t put_question(unsigned char *out, size_t cap, const char *host, uint16_t id)
{
    if (cap < 18 + strlen(host))
        return 0;
    memset(out, 0, 12);
    out[0] = (unsigned char)(id >> 8);
    out[1] = (unsigned char)id;
    out[2] = 0x01; /* recursion desired */
    out[5] = 1;    /* one question */
    size_t at = 12;
    const char *label = host;
    while (*label != '\0')
    {
        const char *dot = strchr(label, '.');
        size_t length = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (length == 0 || length > 63)
            return 0;
        out[at++] = (unsigned char)length;
        memcpy(out + at, label, length);
        at += length;
        label += length;
        if (*label == '.')
            label++;
    }
    out[at++] = 0;
    out[at++] = 0;
    out[at++] = 1; /* type A */
    out[at++] = 0;
    out[at++] = 1; /* class IN */
    return at;
}

/* Steps over a (possibly compressed) name; returns the offset after it, or 0. */
static size_t skip_name(const unsigned char *packet, size_t length, size_t at)
{
    while (at < length)
    {
        unsigned char c = packet[at];
        if (c == 0)
            return at + 1;
        if ((c & 0xC0) == 0xC0)
            return at + 2 <= length ? at + 2 : 0;
        at += (size_t)c + 1;
    }
    return 0;
}

static int read_answers(const unsigned char *packet, size_t length, uint16_t id,
                        uint32_t *addresses, int capacity)
{
    if (length < 12 || packet[0] != (unsigned char)(id >> 8) || packet[1] != (unsigned char)id)
        return 0;
    if ((packet[2] & 0x80) == 0 || (packet[3] & 0x0F) != 0) /* not a reply, or an error */
        return 0;
    unsigned questions = (unsigned)(packet[4] << 8 | packet[5]);
    unsigned answers = (unsigned)(packet[6] << 8 | packet[7]);
    size_t at = 12;
    for (unsigned i = 0; i < questions; i++)
    {
        at = skip_name(packet, length, at);
        if (at == 0 || at + 4 > length)
            return 0;
        at += 4;
    }
    int found = 0;
    for (unsigned i = 0; i < answers && found < capacity; i++)
    {
        at = skip_name(packet, length, at);
        if (at == 0 || at + 10 > length)
            break;
        unsigned type = (unsigned)(packet[at] << 8 | packet[at + 1]);
        unsigned rdlength = (unsigned)(packet[at + 8] << 8 | packet[at + 9]);
        at += 10;
        if (at + rdlength > length)
            break;
        if (type == 1 && rdlength == 4)
            memcpy(&addresses[found++], packet + at, 4);
        at += rdlength;
    }
    return found;
}

static int ask(const char *server, const char *host, uint32_t *addresses, int capacity)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    if (inet_pton(AF_INET, server, &to.sin_addr) != 1)
        return 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    struct timeval timeout = {DNS_TIMEOUT_SECONDS, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    uint16_t id = (uint16_t)(((uintptr_t)&to >> 4) ^ (uintptr_t)host ^ (uint16_t)getpid());
    unsigned char query[DNS_PACKET];
    size_t size = put_question(query, sizeof query, host, id);
    int found = 0;
    for (int attempt = 0; size > 0 && attempt < 2 && found == 0; attempt++)
    {
        if (sendto(fd, query, size, 0, (struct sockaddr *)&to, sizeof to) < 0)
            break;
        unsigned char reply[DNS_PACKET * 4];
        ssize_t got = recv(fd, reply, sizeof reply, 0);
        if (got > 0)
            found = read_answers(reply, (size_t)got, id, addresses, capacity);
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
    }
    close(fd);
    return found;
}

/* Home routers answer DNS on their own address, usually the .1 of the console's
 * network. "Connecting" a UDP socket sends nothing; it only picks the local
 * address, from which the guess is made. */
static int guess_router(char *out, size_t size)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_in far;
    memset(&far, 0, sizeof far);
    far.sin_family = AF_INET;
    far.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &far.sin_addr);
    struct sockaddr_in near;
    socklen_t length = sizeof near;
    int ok = connect(fd, (struct sockaddr *)&far, sizeof far) == 0 &&
             getsockname(fd, (struct sockaddr *)&near, &length) == 0 && near.sin_addr.s_addr != 0;
    close(fd);
    if (!ok)
        return 0;
    uint32_t host = ntohl(near.sin_addr.s_addr);
    if ((host & 0xFF) == 1)
        return 0;
    struct in_addr router = {htonl((host & 0xFFFFFF00u) | 1u)};
    return inet_ntop(AF_INET, &router, out, (socklen_t)size) != NULL;
}

int mt_dns_lookup(const char *host, uint32_t *addresses, int capacity)
{
    if (host == NULL || *host == '\0' || capacity < 1)
        return 0;
    if (g_server[0] != '\0')
    {
        int found = ask(g_server, host, addresses, capacity);
        if (found > 0)
            return found;
    }
    char router[48];
    if (guess_router(router, sizeof router))
    {
        int found = ask(router, host, addresses, capacity);
        if (found > 0)
            return found;
    }
    for (int i = 0; g_fallback[i] != NULL; i++)
    {
        int found = ask(g_fallback[i], host, addresses, capacity);
        if (found > 0)
            return found;
    }
    return 0;
}

#define MAX_ADDRESSES 4

struct entry
{
    struct addrinfo info;
    struct sockaddr_in address;
};

static int port_of(const char *service, int *port)
{
    if (service == NULL || *service == '\0')
    {
        *port = 0;
        return 1;
    }
    char *end = NULL;
    long number = strtol(service, &end, 10);
    if (*end == '\0' && number >= 0 && number <= 65535)
    {
        *port = (int)number;
        return 1;
    }
    if (strcmp(service, "http") == 0)
        *port = 80;
    else if (strcmp(service, "https") == 0)
        *port = 443;
    else
        return 0;
    return 1;
}

int mt_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                   struct addrinfo **result)
{
    *result = NULL;
    int family = hints != NULL ? hints->ai_family : AF_UNSPEC;
    int socktype = hints != NULL && hints->ai_socktype != 0 ? hints->ai_socktype : SOCK_STREAM;
    int protocol = hints != NULL ? hints->ai_protocol : 0;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;
    int port = 0;
    if (!port_of(service, &port))
        return EAI_SERVICE;
    if (node == NULL)
        return EAI_NONAME;

    uint32_t addresses[MAX_ADDRESSES];
    int count = 0;
    struct in_addr numeric;
    if (inet_pton(AF_INET, node, &numeric) == 1)
    {
        addresses[count++] = numeric.s_addr;
    }
    else if (strcmp(node, "localhost") == 0)
    {
        addresses[count++] = htonl(INADDR_LOOPBACK);
    }
    else if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST) != 0)
    {
        return EAI_NONAME;
    }
    else
    {
        count = mt_dns_lookup(node, addresses, MAX_ADDRESSES);
        if (count == 0)
            return EAI_NONAME;
    }

    struct addrinfo *head = NULL;
    struct addrinfo **tail = &head;
    for (int i = 0; i < count; i++)
    {
        struct entry *entry = calloc(1, sizeof *entry);
        if (entry == NULL)
        {
            mt_freeaddrinfo(head);
            return EAI_MEMORY;
        }
        entry->address.sin_family = AF_INET;
        entry->address.sin_port = htons((uint16_t)port);
        entry->address.sin_addr.s_addr = addresses[i];
        entry->info.ai_family = AF_INET;
        entry->info.ai_socktype = socktype;
        entry->info.ai_protocol = protocol;
        entry->info.ai_addrlen = sizeof entry->address;
        entry->info.ai_addr = (struct sockaddr *)&entry->address;
        *tail = &entry->info;
        tail = &entry->info.ai_next;
    }
    *result = head;
    return 0;
}

void mt_freeaddrinfo(struct addrinfo *info)
{
    while (info != NULL)
    {
        struct addrinfo *next = info->ai_next;
        free(info); /* the first member of its allocation */
        info = next;
    }
}
