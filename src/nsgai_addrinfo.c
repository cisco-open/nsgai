/**
 * @file nsgai_addrinfo.c
 * @brief getaddrinfo/getnameinfo implementation backed by c-ares.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * getaddrinfo()/getnameinfo() implementations on top of c-ares,
 * with per-namespace channel construction.
 */

#include "nsgai_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * glibc marks *_ALLOW_UNASSIGNED and *_USE_STD3_ASCII_RULES as deprecated via
 * macro-expansion pragmas.  Preserve support without expanding those macros on
 * glibc by using their ABI-stable bit values.
 */
#if defined(__GLIBC__)
#define NSGAI_AI_IDN_ALLOW_UNASSIGNED_FLAG     0x0100
#define NSGAI_AI_IDN_USE_STD3_ASCII_RULES_FLAG 0x0200
#define NSGAI_NI_IDN_ALLOW_UNASSIGNED_FLAG     64
#define NSGAI_NI_IDN_USE_STD3_ASCII_RULES_FLAG 128
#else
#ifdef AI_IDN_ALLOW_UNASSIGNED
#define NSGAI_AI_IDN_ALLOW_UNASSIGNED_FLAG AI_IDN_ALLOW_UNASSIGNED
#endif   // ifdef AI_IDN_ALLOW_UNASSIGNED
#ifdef AI_IDN_USE_STD3_ASCII_RULES
#define NSGAI_AI_IDN_USE_STD3_ASCII_RULES_FLAG AI_IDN_USE_STD3_ASCII_RULES
#endif   // ifdef AI_IDN_USE_STD3_ASCII_RULES
#ifdef NI_IDN_ALLOW_UNASSIGNED
#define NSGAI_NI_IDN_ALLOW_UNASSIGNED_FLAG NI_IDN_ALLOW_UNASSIGNED
#endif   // ifdef NI_IDN_ALLOW_UNASSIGNED
#ifdef NI_IDN_USE_STD3_ASCII_RULES
#define NSGAI_NI_IDN_USE_STD3_ASCII_RULES_FLAG NI_IDN_USE_STD3_ASCII_RULES
#endif   // ifdef NI_IDN_USE_STD3_ASCII_RULES
#endif   // defined(__GLIBC__)

/* gai_strerror helpers */

const char *
nsgai_gai_strerror_impl(int code)
{
    switch (code) {
        case 0: return "Success";
        case EAI_AGAIN: return "Temporary failure in name resolution";
        case EAI_BADFLAGS: return "Bad value for ai_flags";
        case EAI_FAIL: return "Non-recoverable failure in name resolution";
        case EAI_FAMILY: return "ai_family not supported";
        case EAI_MEMORY: return "Memory allocation failure";
        case EAI_NONAME: return "Name or service not known";
        case EAI_SERVICE: return "Servname not supported for ai_socktype";
        case EAI_SOCKTYPE: return "ai_socktype not supported";
        case EAI_SYSTEM: return "System error";
        case EAI_OVERFLOW: return "Argument buffer overflow";
#ifdef EAI_ADDRFAMILY
        case EAI_ADDRFAMILY: return "Address family not supported";
#endif   // ifdef EAI_ADDRFAMILY
#ifdef EAI_NODATA
        case EAI_NODATA: return "No address associated with hostname";
#endif   // ifdef EAI_NODATA
        default: return "Unknown error";
    }
}

const char *
nsgai_gai_strerror(int errcode)
{
    return nsgai_gai_strerror_impl(errcode);
}

/* c-ares status -> EAI_* */

/**
 * @brief Map c-ares status values to POSIX EAI_* return codes.
 */

int
nsgai_status_to_eai(ares_status_t s)
{
    /* Preset so unrecognised/infrastructure codes return EAI_FAIL without
     * needing a default label.  Every enumerator is listed below so the
     * compiler catches new ares_status_t values at build time (-Wswitch-enum).
     */
    int eai = EAI_FAIL;
    switch (s) {
        case ARES_SUCCESS: eai = 0; break;
        case ARES_ENODATA:
        case ARES_ENOTFOUND:
        case ARES_ENONAME:
        case ARES_EBADNAME: eai = EAI_NONAME; break;
        case ARES_ENOMEM: eai = EAI_MEMORY; break;
        case ARES_ETIMEOUT:
        case ARES_ECANCELLED: eai = EAI_AGAIN; break;
        case ARES_EBADFAMILY: eai = EAI_FAMILY; break;
        case ARES_ESERVICE: eai = EAI_SERVICE; break;
        case ARES_EBADFLAGS: eai = EAI_BADFLAGS; break;
        /* Infrastructure / unrecoverable errors → EAI_FAIL (preset above). */
        case ARES_EFORMERR:
        case ARES_ESERVFAIL:
        case ARES_ENOTIMP:
        case ARES_EREFUSED:
        case ARES_EBADQUERY:
        case ARES_EBADRESP:
        case ARES_ECONNREFUSED:
        case ARES_EOF:
        case ARES_EFILE:
        case ARES_EDESTRUCTION:
        case ARES_EBADSTR:
        case ARES_EBADHINTS:
        case ARES_ENOTINITIALIZED:
        case ARES_ELOADIPHLPAPI:
        case ARES_EADDRGETNETWORKPARAMS:
        case ARES_ENOSERVER: break;
    }
    return eai;
}

/**
 * @brief Translate libc AI_* flags to c-ares ARES_AI_* flags.
 *
 * libc and c-ares share symbolic names but not numeric values.
 */
int
nsgai_map_ai_flags(int libc_flags)
{
    int out = 0;
    if (libc_flags & AI_CANONNAME) {
        out |= ARES_AI_CANONNAME;
    }
    if (libc_flags & AI_NUMERICHOST) {
        out |= ARES_AI_NUMERICHOST;
    }
    if (libc_flags & AI_PASSIVE) {
        out |= ARES_AI_PASSIVE;
    }
    if (libc_flags & AI_NUMERICSERV) {
        out |= ARES_AI_NUMERICSERV;
    }
    if (libc_flags & AI_V4MAPPED) {
        out |= ARES_AI_V4MAPPED;
    }
    if (libc_flags & AI_ALL) {
        out |= ARES_AI_ALL;
    }
    if (libc_flags & AI_ADDRCONFIG) {
        out |= ARES_AI_ADDRCONFIG;
    }
#ifdef AI_IDN
    if (libc_flags & AI_IDN) {
        out |= ARES_AI_IDN;
    }
#endif   // ifdef AI_IDN
#ifdef AI_CANONIDN
    if (libc_flags & AI_CANONIDN) {
        out |= ARES_AI_CANONIDN;
    }
#endif   // ifdef AI_CANONIDN
#ifdef NSGAI_AI_IDN_ALLOW_UNASSIGNED_FLAG
    if (libc_flags & NSGAI_AI_IDN_ALLOW_UNASSIGNED_FLAG) {
        out |= ARES_AI_IDN_ALLOW_UNASSIGNED;
    }
#endif   // ifdef NSGAI_AI_IDN_ALLOW_UNASSIGNED_FLAG
#ifdef NSGAI_AI_IDN_USE_STD3_ASCII_RULES_FLAG
    if (libc_flags & NSGAI_AI_IDN_USE_STD3_ASCII_RULES_FLAG) {
        out |= ARES_AI_IDN_USE_STD3_ASCII_RULES;
    }
#endif   // ifdef NSGAI_AI_IDN_USE_STD3_ASCII_RULES_FLAG
    return out;
}

/**
 * @brief Translate libc NI_* flags to c-ares ARES_NI_* flags.
 *
 * Lookup mode bits are derived by caller from provided host/serv buffers.
 */
int
nsgai_map_ni_flags(int libc_flags)
{
    int out = 0;
    if (libc_flags & NI_NOFQDN) {
        out |= ARES_NI_NOFQDN;
    }
    if (libc_flags & NI_NUMERICHOST) {
        out |= ARES_NI_NUMERICHOST;
    }
    if (libc_flags & NI_NAMEREQD) {
        out |= ARES_NI_NAMEREQD;
    }
    if (libc_flags & NI_NUMERICSERV) {
        out |= ARES_NI_NUMERICSERV;
    }
    if (libc_flags & NI_DGRAM) {
        out |= ARES_NI_DGRAM;
    }
#ifdef NI_IDN
    if (libc_flags & NI_IDN) {
        out |= ARES_NI_IDN;
    }
#endif   // ifdef NI_IDN
#ifdef NSGAI_NI_IDN_ALLOW_UNASSIGNED_FLAG
    if (libc_flags & NSGAI_NI_IDN_ALLOW_UNASSIGNED_FLAG) {
        out |= ARES_NI_IDN_ALLOW_UNASSIGNED;
    }
#endif   // ifdef NSGAI_NI_IDN_ALLOW_UNASSIGNED_FLAG
#ifdef NSGAI_NI_IDN_USE_STD3_ASCII_RULES_FLAG
    if (libc_flags & NSGAI_NI_IDN_USE_STD3_ASCII_RULES_FLAG) {
        out |= ARES_NI_IDN_USE_STD3_ASCII_RULES;
    }
#endif   // ifdef NSGAI_NI_IDN_USE_STD3_ASCII_RULES_FLAG
    return out;
}

/**
 * @brief Resolve service name or numeric port to network-order port.
 *
 * /etc/services is process-global (not netns-specific), so libc lookup is
 * used directly and optionally constrained by ai_socktype.
 */

static int
resolve_service(
    const char *service, const struct addrinfo *hints, uint16_t *out_port)
{
    if (!service || !*service) {
        *out_port = 0;
        return 0;
    }
    /* Try numeric first. */
    char *end       = NULL;
    unsigned long v = strtoul(service, &end, 10);
    if (end && *end == '\0') {
        if (v > 0xffff) {
            return EAI_SERVICE;
        }
        *out_port = htons((uint16_t)v);
        return 0;
    }
    if (hints && (hints->ai_flags & AI_NUMERICSERV)) {
        return EAI_NONAME;
    }

    const char *proto = NULL;
    if (hints && hints->ai_socktype == SOCK_STREAM) {
        proto = "tcp";
    } else if (hints && hints->ai_socktype == SOCK_DGRAM) {
        proto = "udp";
    }

    struct servent se, *result = NULL;
    char buf[1024];
    if (getservbyname_r(service, proto, &se, buf, sizeof(buf), &result) != 0 ||
        result == NULL) {
        return EAI_SERVICE;
    }
    *out_port = (uint16_t)result->s_port;
    return 0;
}

/* Result conversion: ares_addrinfo -> addrinfo chain */

static struct addrinfo *
new_ai_node(int family,
    int socktype,
    int protocol,
    const void *sa_data,
    size_t sa_len,
    uint16_t port_n,
    const char *canonname)
{
    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(*ai));
    if (!ai) {
        return NULL;
    }
    ai->ai_family   = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;

    if (family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(*sin));
        if (!sin) {
            free(ai);
            return NULL;
        }
        sin->sin_family = AF_INET;
        sin->sin_port   = port_n;
        memcpy(&sin->sin_addr, sa_data, sa_len);
        ai->ai_addr    = (struct sockaddr *)sin;
        ai->ai_addrlen = sizeof(*sin);
    } else {
        struct sockaddr_in6 *sin6 =
            (struct sockaddr_in6 *)calloc(1, sizeof(*sin6));
        if (!sin6) {
            free(ai);
            return NULL;
        }
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = port_n;
        memcpy(&sin6->sin6_addr, sa_data, sa_len);
        ai->ai_addr    = (struct sockaddr *)sin6;
        ai->ai_addrlen = sizeof(*sin6);
    }

    if (canonname) {
        ai->ai_canonname = strdup(canonname);
        if (!ai->ai_canonname) {
            free(ai->ai_addr);
            free(ai);
            return NULL;
        }
    }
    return ai;
}

int
nsgai_convert_addrinfo(const struct ares_addrinfo *src,
    const struct addrinfo *hints,
    const char *service,
    struct addrinfo **out)
{
    *out = NULL;
    if (!src || !src->nodes) {
        return EAI_NONAME;
    }

    uint16_t port_n = 0;
    int rc          = resolve_service(service, hints, &port_n);
    if (rc != 0) {
        return rc;
    }

    const char *canon = NULL;
    if (hints && (hints->ai_flags & AI_CANONNAME)) {
        if (src->cnames && src->cnames->name) {
            canon = src->cnames->name;
        } else {
            canon = src->name;
        }
    }

    struct addrinfo *head = NULL, *tail = NULL;
    for (const struct ares_addrinfo_node *n = src->nodes; n; n = n->ai_next) {
        const void *adata;
        size_t alen;
        if (n->ai_family == AF_INET) {
            adata = &((struct sockaddr_in *)n->ai_addr)->sin_addr;
            alen  = sizeof(struct in_addr);
        } else if (n->ai_family == AF_INET6) {
            adata = &((struct sockaddr_in6 *)n->ai_addr)->sin6_addr;
            alen  = sizeof(struct in6_addr);
        } else {
            continue;
        }
        int st = n->ai_socktype ?: (hints ? hints->ai_socktype : 0);
        int pr = n->ai_protocol ?: (hints ? hints->ai_protocol : 0);

        struct addrinfo *node = new_ai_node(n->ai_family, st, pr, adata, alen,
            port_n, (head == NULL) ? canon : NULL);
        if (!node) {
            nsgai_freeaddrinfo(head);
            return EAI_MEMORY;
        }
        if (!head) {
            head = node;
        } else {
            tail->ai_next = node;
        }
        tail = node;
    }

    if (!head) {
        return EAI_NONAME;
    }
    *out = head;
    return 0;
}

static int
nsgai_synthesize_null_node_addrinfo(
    const char *service, const struct addrinfo *hints, struct addrinfo **out)
{
    *out = NULL;

    /* POSIX getaddrinfo requires at least one of node/service. */
    if (service == NULL) {
        return EAI_NONAME;
    }

    int family = hints ? hints->ai_family : AF_UNSPEC;
    if (family != AF_INET && family != AF_INET6 && family != AF_UNSPEC) {
        return EAI_FAMILY;
    }

    uint16_t port_n = 0;
    int rc          = resolve_service(service, hints, &port_n);
    if (rc != 0) {
        return rc;
    }

    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    int passive  = (hints && (hints->ai_flags & AI_PASSIVE));

    struct addrinfo *head = NULL;
    struct addrinfo *tail = NULL;

    if (family == AF_INET || family == AF_UNSPEC) {
        struct in_addr v4;
        v4.s_addr             = htonl(passive ? INADDR_ANY : INADDR_LOOPBACK);
        struct addrinfo *node = new_ai_node(
            AF_INET, socktype, protocol, &v4, sizeof(v4), port_n, NULL);
        if (!node) {
            return EAI_MEMORY;
        }
        head = tail = node;
    }

    if (family == AF_INET6 || family == AF_UNSPEC) {
        struct in6_addr v6    = passive ? in6addr_any : in6addr_loopback;
        struct addrinfo *node = new_ai_node(
            AF_INET6, socktype, protocol, &v6, sizeof(v6), port_n, NULL);
        if (!node) {
            nsgai_freeaddrinfo(head);
            return EAI_MEMORY;
        }
        if (!head) {
            head = node;
        } else {
            tail->ai_next = node;
        }
        tail = node;
    }

    if (!head) {
        return EAI_NONAME;
    }

    *out = head;
    return 0;
}

/**
 * @brief Free addrinfo chain allocated by nsgai conversion helpers.
 */

void
nsgai_freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

static const char *
nsgai_hosts_path(const nsgai_ns_t *ns)
{
    if (ns != NULL && ns->hosts_path != NULL) {
        return ns->hosts_path;
    }
    return "/etc/hosts";
}

static bool
nsgai_policy_allows_hosts(nsgai_lookup_policy_t policy)
{
    return policy == NSGAI_LOOKUP_HOSTS_FIRST ||
           policy == NSGAI_LOOKUP_DNS_FIRST ||
           policy == NSGAI_LOOKUP_HOSTS_ONLY;
}

static bool
nsgai_policy_hosts_first(nsgai_lookup_policy_t policy)
{
    return policy == NSGAI_LOOKUP_HOSTS_FIRST ||
           policy == NSGAI_LOOKUP_HOSTS_ONLY;
}

static bool
nsgai_policy_allows_dns(nsgai_lookup_policy_t policy)
{
    return policy == NSGAI_LOOKUP_HOSTS_FIRST ||
           policy == NSGAI_LOOKUP_DNS_FIRST || policy == NSGAI_LOOKUP_DNS_ONLY;
}

static bool
nsgai_policy_is_valid(nsgai_lookup_policy_t policy)
{
    switch (policy) {
        case NSGAI_LOOKUP_HOSTS_FIRST:
        case NSGAI_LOOKUP_DNS_FIRST:
        case NSGAI_LOOKUP_HOSTS_ONLY:
        case NSGAI_LOOKUP_DNS_ONLY: return true;
    }
    return false;
}

static _Atomic int g_lookup_policy = NSGAI_LOOKUP_HOSTS_FIRST;

int
nsgai_set_lookup_policy(nsgai_lookup_policy_t policy)
{
    if (!nsgai_policy_is_valid(policy)) {
        return EAI_BADFLAGS;
    }
    atomic_store_explicit(&g_lookup_policy, (int)policy, memory_order_relaxed);
    return 0;
}

static nsgai_lookup_policy_t
nsgai_get_lookup_policy(void)
{
    return (nsgai_lookup_policy_t)atomic_load_explicit(
        &g_lookup_policy, memory_order_relaxed);
}

static bool
nsgai_addr_family_matches(int wanted, int actual)
{
    return wanted == AF_UNSPEC || wanted == actual;
}

static void
nsgai_hosts_strip_comment(char *line)
{
    for (char *p = line; *p != '\0'; ++p) {
        if (*p == '#') {
            *p = '\0';
            break;
        }
    }
}

static int
nsgai_getline_token(char **cursor, char **out)
{
    char *p = *cursor;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p == '\0') {
        *out    = NULL;
        *cursor = p;
        return 0;
    }
    char *start = p;
    while (*p != '\0' && !isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    *out    = start;
    *cursor = p;
    return 1;
}

static int
nsgai_append_host_addrinfo(struct addrinfo **head,
    struct addrinfo **tail,
    int family,
    const void *addr,
    size_t addrlen,
    uint16_t port_n,
    const struct addrinfo *hints,
    const char *canonname)
{
    int socktype          = hints ? hints->ai_socktype : 0;
    int protocol          = hints ? hints->ai_protocol : 0;
    struct addrinfo *node = new_ai_node(
        family, socktype, protocol, addr, addrlen, port_n, canonname);
    if (node == NULL) {
        return EAI_MEMORY;
    }
    if (*head == NULL) {
        *head = node;
    } else {
        (*tail)->ai_next = node;
    }
    *tail = node;
    return 0;
}

static int
nsgai_lookup_hosts_forward(const nsgai_ns_t *ns,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **out)
{
    *out     = NULL;
    FILE *fp = fopen(nsgai_hosts_path(ns), "r");
    if (fp == NULL) {
        return EAI_NONAME;
    }

    uint16_t port_n = 0;
    int rc          = resolve_service(service, hints, &port_n);
    if (rc != 0) {
        fclose(fp);
        return rc;
    }

    struct addrinfo *head = NULL;
    struct addrinfo *tail = NULL;
    char *line            = NULL;
    size_t cap            = 0;
    int family_wanted     = hints ? hints->ai_family : AF_UNSPEC;
    int want_canon        = hints && (hints->ai_flags & AI_CANONNAME);

    while (getline(&line, &cap, fp) >= 0) {
        nsgai_hosts_strip_comment(line);
        char *cursor = line;
        char *addr_s = NULL;
        if (!nsgai_getline_token(&cursor, &addr_s)) {
            continue;
        }

        struct in_addr addr4;
        struct in6_addr addr6;
        int family = AF_UNSPEC;
        const void *addrptr;
        size_t addrlen;
        if (inet_pton(AF_INET, addr_s, &addr4) == 1) {
            family  = AF_INET;
            addrptr = &addr4;
            addrlen = sizeof(addr4);
        } else if (inet_pton(AF_INET6, addr_s, &addr6) == 1) {
            family  = AF_INET6;
            addrptr = &addr6;
            addrlen = sizeof(addr6);
        } else {
            continue;
        }
        if (!nsgai_addr_family_matches(family_wanted, family)) {
            continue;
        }

        char *host_tok = NULL;
        char *canon    = NULL;
        int matched    = 0;
        while (nsgai_getline_token(&cursor, &host_tok)) {
            if (canon == NULL) {
                canon = host_tok;
            }
            if (strcasecmp(host_tok, node) == 0) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            continue;
        }

        rc = nsgai_append_host_addrinfo(&head, &tail, family, addrptr, addrlen,
            port_n, hints, (head == NULL && want_canon) ? canon : NULL);
        if (rc != 0) {
            nsgai_freeaddrinfo(head);
            free(line);
            fclose(fp);
            return rc;
        }
    }

    free(line);
    fclose(fp);
    if (head == NULL) {
        return EAI_NONAME;
    }
    *out = head;
    return 0;
}

static int
nsgai_trim_no_fqdn(char *host)
{
    char *dot = strchr(host, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    return 0;
}

static int
nsgai_lookup_hosts_reverse(const nsgai_ns_t *ns,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    int flags)
{
    (void)salen;
    FILE *fp = fopen(nsgai_hosts_path(ns), "r");
    if (fp == NULL) {
        return EAI_NONAME;
    }

    int family = sa->sa_family;
    char *line = NULL;
    size_t cap = 0;
    int rc     = EAI_NONAME;

    while (getline(&line, &cap, fp) >= 0) {
        nsgai_hosts_strip_comment(line);
        char *cursor = line;
        char *addr_s = NULL;
        if (!nsgai_getline_token(&cursor, &addr_s)) {
            continue;
        }

        int matched = 0;
        if (family == AF_INET) {
            struct in_addr want = ((const struct sockaddr_in *)sa)->sin_addr;
            struct in_addr got;
            matched = inet_pton(AF_INET, addr_s, &got) == 1 &&
                      memcmp(&want, &got, sizeof(got)) == 0;
        } else if (family == AF_INET6) {
            struct in6_addr want = ((const struct sockaddr_in6 *)sa)->sin6_addr;
            struct in6_addr got;
            matched = inet_pton(AF_INET6, addr_s, &got) == 1 &&
                      memcmp(&want, &got, sizeof(got)) == 0;
        }
        if (!matched) {
            continue;
        }

        char *host_tok = NULL;
        if (!nsgai_getline_token(&cursor, &host_tok) || host_tok == NULL) {
            continue;
        }
        if (flags & NI_NOFQDN) {
            nsgai_trim_no_fqdn(host_tok);
        }
        size_t need = strlen(host_tok) + 1;
        if (need > hostlen) {
            rc = EAI_OVERFLOW;
        } else {
            memcpy(host, host_tok, need);
            rc = 0;
        }
        break;
    }

    free(line);
    fclose(fp);
    return rc;
}

static int
nsgai_numeric_node_to_addrinfo(const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **out)
{
    *out = NULL;

    int family_wanted = hints ? hints->ai_family : AF_UNSPEC;
    uint16_t port_n   = 0;
    int rc            = resolve_service(service, hints, &port_n);
    if (rc != 0) {
        return rc;
    }

    struct addrinfo *head = NULL;
    struct addrinfo *tail = NULL;
    struct in_addr addr4;
    if (nsgai_addr_family_matches(family_wanted, AF_INET) &&
        inet_pton(AF_INET, node, &addr4) == 1) {
        rc = nsgai_append_host_addrinfo(
            &head, &tail, AF_INET, &addr4, sizeof(addr4), port_n, hints, NULL);
        if (rc != 0) {
            return rc;
        }
    }

    struct in6_addr addr6;
    if (nsgai_addr_family_matches(family_wanted, AF_INET6) &&
        inet_pton(AF_INET6, node, &addr6) == 1) {
        rc = nsgai_append_host_addrinfo(
            &head, &tail, AF_INET6, &addr6, sizeof(addr6), port_n, hints, NULL);
        if (rc != 0) {
            nsgai_freeaddrinfo(head);
            return rc;
        }
    }

    if (head == NULL) {
        return EAI_NONAME;
    }
    *out = head;
    return 0;
}

static int
nsgai_format_numeric_host(
    const struct sockaddr *sa, char *host, socklen_t hostlen)
{
    const void *addr = NULL;
    if (sa->sa_family == AF_INET) {
        addr = &((const struct sockaddr_in *)sa)->sin_addr;
    } else if (sa->sa_family == AF_INET6) {
        addr = &((const struct sockaddr_in6 *)sa)->sin6_addr;
    } else {
        return EAI_FAMILY;
    }
    return inet_ntop(sa->sa_family, addr, host, hostlen) != NULL ? 0
                                                                 : EAI_OVERFLOW;
}

static int
nsgai_format_service(
    const struct sockaddr *sa, char *serv, socklen_t servlen, int flags)
{
    if (serv == NULL || servlen == 0) {
        return 0;
    }

    uint16_t port_n = 0;
    if (sa->sa_family == AF_INET) {
        port_n = ((const struct sockaddr_in *)sa)->sin_port;
    } else if (sa->sa_family == AF_INET6) {
        port_n = ((const struct sockaddr_in6 *)sa)->sin6_port;
    } else {
        return EAI_FAMILY;
    }

    unsigned port = ntohs(port_n);
    if (flags & NI_NUMERICSERV) {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%u", port);
        if (n < 0 || (size_t)n + 1 > servlen) {
            return EAI_OVERFLOW;
        }
        memcpy(serv, buf, (size_t)n + 1);
        return 0;
    }

    const char *proto = (flags & NI_DGRAM) ? "udp" : "tcp";
    struct servent se;
    struct servent *result = NULL;
    char buf[1024];
    if (getservbyport_r((int)port_n, proto, &se, buf, sizeof(buf), &result) !=
            0 ||
        result == NULL) {
        char nbuf[16];
        int n = snprintf(nbuf, sizeof(nbuf), "%u", port);
        if (n < 0 || (size_t)n + 1 > servlen) {
            return EAI_OVERFLOW;
        }
        memcpy(serv, nbuf, (size_t)n + 1);
        return 0;
    }

    size_t need = strlen(result->s_name) + 1;
    if (need > servlen) {
        return EAI_OVERFLOW;
    }
    memcpy(serv, result->s_name, need);
    return 0;
}

/* Synchronous wait infrastructure */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
    int status; /* ares status */
    int timeouts;
    struct ares_addrinfo *result; /* for getaddrinfo */
    char *ni_node;                /* for getnameinfo */
    char *ni_service;
} sync_t;

static int
sync_init(sync_t *s)
{
    int rc =
        nsgai_pthread_mutex_init_checked(&s->mu, "pthread_mutex_init(sync.mu)");
    if (rc != 0) {
        return rc;
    }
    rc = nsgai_pthread_cond_init_checked(&s->cv, "pthread_cond_init(sync.cv)");
    if (rc != 0) {
        nsgai_pthread_mutex_destroy_checked(
            &s->mu, "pthread_mutex_destroy(sync.mu cleanup)");
        return rc;
    }
    s->done       = 0;
    s->status     = ARES_SUCCESS;
    s->timeouts   = 0;
    s->result     = NULL;
    s->ni_node    = NULL;
    s->ni_service = NULL;
    return 0;
}

static void
sync_destroy(sync_t *s)
{
    nsgai_pthread_mutex_destroy_checked(
        &s->mu, "pthread_mutex_destroy(sync.mu)");
    nsgai_pthread_cond_destroy_checked(&s->cv, "pthread_cond_destroy(sync.cv)");
}

static void
ai_callback(void *arg, int status, int timeouts, struct ares_addrinfo *result)
{
    sync_t *s = (sync_t *)arg;
    nsgai_pthread_mutex_lock_or_abort(&s->mu, "pthread_mutex_lock(sync.mu)");
    s->status   = status;
    s->timeouts = timeouts;
    s->result   = result;
    s->done     = 1;
    nsgai_pthread_cond_signal_or_abort(&s->cv, "pthread_cond_signal(sync.cv)");
    nsgai_pthread_mutex_unlock_or_abort(
        &s->mu, "pthread_mutex_unlock(sync.mu)");
}

static void
ni_callback(
    void *arg, int status, int timeouts, const char *node, const char *service)
{
    sync_t *s = (sync_t *)arg;
    nsgai_pthread_mutex_lock_or_abort(&s->mu, "pthread_mutex_lock(sync.mu)");
    s->status     = status;
    s->timeouts   = timeouts;
    s->ni_node    = node ? strdup(node) : NULL;
    s->ni_service = service ? strdup(service) : NULL;
    s->done       = 1;
    nsgai_pthread_cond_signal_or_abort(&s->cv, "pthread_cond_signal(sync.cv)");
    nsgai_pthread_mutex_unlock_or_abort(
        &s->mu, "pthread_mutex_unlock(sync.mu)");
}

static void
sync_wait(sync_t *s, ares_channel_t *channel)
{
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    (void)channel;
    nsgai_pthread_mutex_lock_or_abort(&s->mu, "pthread_mutex_lock(sync.mu)");
    while (!s->done) {
        nsgai_pthread_cond_wait_or_abort(
            &s->cv, &s->mu, "pthread_cond_wait(sync.cv)");
    }
    nsgai_pthread_mutex_unlock_or_abort(
        &s->mu, "pthread_mutex_unlock(sync.mu)");
#else    // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    for (;;) {
        nsgai_pthread_mutex_lock_or_abort(
            &s->mu, "pthread_mutex_lock(sync.mu)");
        int done = s->done;
        nsgai_pthread_mutex_unlock_or_abort(
            &s->mu, "pthread_mutex_unlock(sync.mu)");
        if (done) {
            break;
        }

        ares_socket_t socks[ARES_GETSOCK_MAXNUM];
        int bitmask = ares_getsock(channel, socks, ARES_GETSOCK_MAXNUM);

        struct pollfd pfds[ARES_GETSOCK_MAXNUM];
        int sock_index[ARES_GETSOCK_MAXNUM];
        nfds_t nfds = 0;
        for (int i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
            if (socks[i] == ARES_SOCKET_BAD) {
                continue;
            }
            short events = 0;
            if (ARES_GETSOCK_READABLE(bitmask, i)) {
                events |= POLLIN;
            }
            if (ARES_GETSOCK_WRITABLE(bitmask, i)) {
                events |= POLLOUT;
            }
            if (!events) {
                continue;
            }
            pfds[nfds].fd      = (int)socks[i];
            pfds[nfds].events  = events;
            pfds[nfds].revents = 0;
            sock_index[nfds]   = i;
            nfds++;
        }

        struct timeval tv;
        struct timeval *tvp = ares_timeout(channel, NULL, &tv);
        int timeout_ms      = -1;
        if (tvp) {
            long long ms = (long long)tvp->tv_sec * 1000 +
                           ((long long)tvp->tv_usec + 999) / 1000;
            if (ms < 0) {
                ms = 0;
            }
            if (ms > INT_MAX) {
                ms = INT_MAX;
            }
            timeout_ms = (int)ms;
        }

        int poll_rc = poll(nfds ? pfds : NULL, nfds, timeout_ms);

        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* Let c-ares progress timeout/error handling on transient wait
             * failures rather than aborting the whole lookup immediately. */
            ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
            continue;
        }

        if (poll_rc == 0) {
            ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
            continue;
        }

        for (nfds_t j = 0; j < nfds; j++) {
            short revents = pfds[j].revents;
            if (!revents) {
                continue;
            }
            int i             = sock_index[j];
            ares_socket_t rfd = ARES_SOCKET_BAD;
            ares_socket_t wfd = ARES_SOCKET_BAD;
            if (ARES_GETSOCK_READABLE(bitmask, i) &&
                (revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
                rfd = socks[i];
            }
            if (ARES_GETSOCK_WRITABLE(bitmask, i) &&
                (revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
                wfd = socks[i];
            }
            if (rfd != ARES_SOCKET_BAD || wfd != ARES_SOCKET_BAD) {
                ares_process_fd(channel, rfd, wfd);
            }
        }
    }
#endif   // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX
}

/**
 * @brief Core getaddrinfo workflow for a resolved namespace descriptor.
 */

static int
do_dns_getaddrinfo(const nsgai_ns_t *ns,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res)
{
    if (node == NULL) {
        return nsgai_synthesize_null_node_addrinfo(service, hints, res);
    }

    nsgai_ns_t dns_ns = *ns;
    dns_ns.hosts_path = NULL;

    nsgai_channel_t *ch = NULL;
    int rc              = nsgai_channel_create(&dns_ns, &ch);
    if (rc != ARES_SUCCESS) {
        if (rc == ARES_ENOMEM) {
            return EAI_MEMORY;
        }
        return EAI_SYSTEM;
    }

    struct ares_addrinfo_hints ah;
    memset(&ah, 0, sizeof(ah));
    if (hints) {
        ah.ai_flags    = nsgai_map_ai_flags(hints->ai_flags);
        ah.ai_family   = hints->ai_family;
        ah.ai_socktype = hints->ai_socktype;
        ah.ai_protocol = hints->ai_protocol;
    } else {
        ah.ai_family = AF_UNSPEC;
    }

    sync_t s;
    rc = sync_init(&s);
    if (rc != 0) {
        nsgai_channel_destroy(ch);
        return (rc == ENOMEM) ? EAI_MEMORY : EAI_SYSTEM;
    }

    ares_getaddrinfo(
        nsgai_channel_ares(ch), node, service, &ah, ai_callback, &s);
    sync_wait(&s, nsgai_channel_ares(ch));

    int eai = 0;
    if (s.status != ARES_SUCCESS) {
        eai = nsgai_status_to_eai((ares_status_t)s.status);
    } else {
        eai = nsgai_convert_addrinfo(s.result, hints, service, res);
    }
    if (s.result) {
        ares_freeaddrinfo(s.result);
    }
    sync_destroy(&s);
    nsgai_channel_destroy(ch);
    return eai;
}

static int
do_getaddrinfo(const nsgai_ns_t *ns,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    nsgai_lookup_policy_t policy,
    struct addrinfo **res)
{
    *res = NULL;
    if (node == NULL) {
        return nsgai_synthesize_null_node_addrinfo(service, hints, res);
    }

    int rc = nsgai_numeric_node_to_addrinfo(node, service, hints, res);
    if (rc == 0) {
        return 0;
    }
    if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST)) {
        return rc;
    }
    if (rc != EAI_NONAME) {
        return rc;
    }

    if (nsgai_policy_hosts_first(policy) && nsgai_policy_allows_hosts(policy)) {
        rc = nsgai_lookup_hosts_forward(ns, node, service, hints, res);
        if (rc == 0 || policy == NSGAI_LOOKUP_HOSTS_ONLY) {
            return rc;
        }
    }

    if (nsgai_policy_allows_dns(policy)) {
        rc = do_dns_getaddrinfo(ns, node, service, hints, res);
        if (rc == 0 || !nsgai_policy_allows_hosts(policy) ||
            policy == NSGAI_LOOKUP_HOSTS_FIRST) {
            return rc;
        }
    } else {
        return EAI_NONAME;
    }

    return nsgai_lookup_hosts_forward(ns, node, service, hints, res);
}

/* Public getaddrinfo API */

int
nsgai_getaddrinfo(const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res)
{
    nsgai_ns_t ns;
    if (nsgai_ns_resolve_implicit(&ns) != 0) {
        return EAI_SYSTEM;
    }
    nsgai_lookup_policy_t policy = nsgai_get_lookup_policy();
    int rc = do_getaddrinfo(&ns, node, service, hints, policy, res);
    nsgai_ns_release(&ns);
    return rc;
}

int
nsgai_getaddrinfo_ns(const char *ns_name,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res)
{
    nsgai_ns_t ns;
    /* NULL namespace means resolve in the current (caller's) namespace. */
    if (ns_name == NULL) {
        if (nsgai_ns_resolve_implicit(&ns) != 0) {
            return EAI_SYSTEM;
        }
    } else {
        if (nsgai_ns_resolve_explicit_name(ns_name, &ns) != 0) {
            return EAI_SYSTEM;
        }
    }
    nsgai_lookup_policy_t policy = nsgai_get_lookup_policy();
    int rc = do_getaddrinfo(&ns, node, service, hints, policy, res);
    nsgai_ns_release(&ns);
    return rc;
}

int
nsgai_getaddrinfo_nsfd(int ns_fd,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res)
{
    nsgai_ns_t ns;
    /* Negative fd means resolve in the current (caller's) namespace. */
    if (ns_fd < 0) {
        if (nsgai_ns_resolve_implicit(&ns) != 0) {
            return EAI_SYSTEM;
        }
    } else {
        if (nsgai_ns_resolve_explicit_fd(ns_fd, &ns) != 0) {
            return EAI_SYSTEM;
        }
    }
    nsgai_lookup_policy_t policy = nsgai_get_lookup_policy();
    int rc = do_getaddrinfo(&ns, node, service, hints, policy, res);
    nsgai_ns_release(&ns);
    return rc;
}

/* getnameinfo implementation */

static int
do_dns_getnameinfo(const nsgai_ns_t *ns,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags)
{
    nsgai_ns_t dns_ns = *ns;
    dns_ns.hosts_path = NULL;

    nsgai_channel_t *ch = NULL;
    int rc              = nsgai_channel_create(&dns_ns, &ch);
    if (rc != ARES_SUCCESS) {
        return (rc == ARES_ENOMEM) ? EAI_MEMORY : EAI_SYSTEM;
    }

    sync_t s;
    rc = sync_init(&s);
    if (rc != 0) {
        nsgai_channel_destroy(ch);
        return (rc == ENOMEM) ? EAI_MEMORY : EAI_SYSTEM;
    }

    /* Translate libc NI_* to c-ares ARES_NI_* (numeric values differ),
     * then add the lookup-type bits based on which buffers the caller
     * provided. */
    int ares_flags = nsgai_map_ni_flags(flags);
    if (host && hostlen > 0) {
        ares_flags |= ARES_NI_LOOKUPHOST;
    }
    if (serv && servlen > 0) {
        ares_flags |= ARES_NI_LOOKUPSERVICE;
    }

    ares_getnameinfo(nsgai_channel_ares(ch), sa, salen, ares_flags,
        (ares_nameinfo_callback)ni_callback, &s);
    sync_wait(&s, nsgai_channel_ares(ch));

    int eai = 0;
    if (s.status != ARES_SUCCESS) {
        eai = nsgai_status_to_eai((ares_status_t)s.status);
    } else {
        if (host && hostlen > 0 && s.ni_node) {
            if (strlen(s.ni_node) + 1 > hostlen) {
                eai = EAI_OVERFLOW;
            } else {
                strcpy(host, s.ni_node);
            }
        }
        if (eai == 0 && serv && servlen > 0 && s.ni_service) {
            if (strlen(s.ni_service) + 1 > servlen) {
                eai = EAI_OVERFLOW;
            } else {
                strcpy(serv, s.ni_service);
            }
        }
    }

    free(s.ni_node);
    free(s.ni_service);
    sync_destroy(&s);
    nsgai_channel_destroy(ch);
    return eai;
}

static int
do_getnameinfo(const nsgai_ns_t *ns,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags,
    nsgai_lookup_policy_t policy)
{
    int rc = nsgai_format_service(sa, serv, servlen, flags);
    if (rc != 0) {
        return rc;
    }

    if (host == NULL || hostlen == 0) {
        return 0;
    }
    if (flags & NI_NUMERICHOST) {
        return nsgai_format_numeric_host(sa, host, hostlen);
    }

    if (nsgai_policy_hosts_first(policy) && nsgai_policy_allows_hosts(policy)) {
        rc = nsgai_lookup_hosts_reverse(ns, sa, salen, host, hostlen, flags);
        if (rc == 0 || policy == NSGAI_LOOKUP_HOSTS_ONLY) {
            return rc;
        }
    }

    if (nsgai_policy_allows_dns(policy)) {
        rc = do_dns_getnameinfo(
            ns, sa, salen, host, hostlen, NULL, 0, flags | NI_NUMERICSERV);
        if (rc == 0 || !nsgai_policy_allows_hosts(policy) ||
            policy == NSGAI_LOOKUP_HOSTS_FIRST) {
            return rc;
        }
    } else {
        return (flags & NI_NAMEREQD)
                   ? EAI_NONAME
                   : nsgai_format_numeric_host(sa, host, hostlen);
    }

    rc = nsgai_lookup_hosts_reverse(ns, sa, salen, host, hostlen, flags);
    if (rc == 0) {
        return 0;
    }
    return (flags & NI_NAMEREQD) ? rc
                                 : nsgai_format_numeric_host(sa, host, hostlen);
}

int
nsgai_getnameinfo(const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags)
{
    nsgai_ns_t ns;
    if (nsgai_ns_resolve_implicit(&ns) != 0) {
        return EAI_SYSTEM;
    }
    nsgai_lookup_policy_t policy = nsgai_get_lookup_policy();
    int rc                       = do_getnameinfo(
        &ns, sa, salen, host, hostlen, serv, servlen, flags, policy);
    nsgai_ns_release(&ns);
    return rc;
}

int
nsgai_getnameinfo_ns(const char *ns_name,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags)
{
    nsgai_ns_t ns;
    /* NULL namespace means resolve in the current (caller's) namespace. */
    if (ns_name == NULL) {
        if (nsgai_ns_resolve_implicit(&ns) != 0) {
            return EAI_SYSTEM;
        }
    } else {
        if (nsgai_ns_resolve_explicit_name(ns_name, &ns) != 0) {
            return EAI_SYSTEM;
        }
    }
    nsgai_lookup_policy_t policy = nsgai_get_lookup_policy();
    int rc                       = do_getnameinfo(
        &ns, sa, salen, host, hostlen, serv, servlen, flags, policy);
    nsgai_ns_release(&ns);
    return rc;
}

int
nsgai_getnameinfo_nsfd(int ns_fd,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags)
{
    nsgai_ns_t ns;
    /* Negative fd means resolve in the current (caller's) namespace. */
    if (ns_fd < 0) {
        if (nsgai_ns_resolve_implicit(&ns) != 0) {
            return EAI_SYSTEM;
        }
    } else {
        if (nsgai_ns_resolve_explicit_fd(ns_fd, &ns) != 0) {
            return EAI_SYSTEM;
        }
    }
    nsgai_lookup_policy_t policy = nsgai_get_lookup_policy();
    int rc                       = do_getnameinfo(
        &ns, sa, salen, host, hostlen, serv, servlen, flags, policy);
    nsgai_ns_release(&ns);
    return rc;
}

/**
 * @brief POSIX symbol aliases exported with weak linkage.
 *
 * These allow preload/link-order interception while remaining overrideable.
 */

__attribute__((weak, visibility("default"))) int
getaddrinfo(const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res)
{
    return nsgai_getaddrinfo(node, service, hints, res);
}

__attribute__((weak, visibility("default"))) void
freeaddrinfo(struct addrinfo *res)
{
    nsgai_freeaddrinfo(res);
}

__attribute__((weak, visibility("default"))) const char *
gai_strerror(int errcode)
{
    return nsgai_gai_strerror_impl(errcode);
}

__attribute__((weak, visibility("default"))) int
getnameinfo(const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags)
{
    return nsgai_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
}
