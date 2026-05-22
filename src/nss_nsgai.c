/**
 * @file nss_nsgai.c
 * @brief NSS hosts backend powered by nsgai and c-ares.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * libnss_nsgai.so.2 - NSS module.
 *
 * Registered in /etc/nsswitch.conf as:
 *      hosts: files nsgai dns
 *
 * On every call we resolve the namespace selection from thread state
 * (TLS override / NSGAI_NS{_FD} env), build a per-call c-ares channel
 * and copy the result into the caller-supplied buffer per the NSS
 * reentrant calling convention.
 *
 * Implemented entry points:
 *   _nss_nsgai_gethostbyname2_r
 *   _nss_nsgai_gethostbyname_r   (forwards to gethostbyname2_r)
 *   _nss_nsgai_gethostbyaddr_r
 *   _nss_nsgai_gethostbyname4_r  (preferred, returns gaih_addrtuple)
 */

#include "nsgai.h"
#include "nsgai_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <nss.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/**
 * @brief ABI guardrails for glibc-provided gaih_addrtuple.
 */
_Static_assert(sizeof(((struct gaih_addrtuple *)0)->addr) == 16,
    "gaih_addrtuple.addr must be 16 bytes (4 x uint32_t)");
_Static_assert(offsetof(struct gaih_addrtuple, next) == 0,
    "gaih_addrtuple.next must be first member");

static void
nsgai__nss_ai_cb(
    void *arg, int status, int timeouts, struct ares_addrinfo *result);

/**
 * @brief Local synchronization object for synchronous c-ares waits.
 */
typedef struct {
    int done;
    int status;
    struct ares_addrinfo *res;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} nss_sync_t;

static int
nss_sync_init(nss_sync_t *s)
{
    int rc = nsgai_pthread_mutex_init_checked(
        &s->mu, "pthread_mutex_init(nss_sync.mu)");
    if (rc != 0) {
        return rc;
    }
    rc = nsgai_pthread_cond_init_checked(
        &s->cv, "pthread_cond_init(nss_sync.cv)");
    if (rc != 0) {
        nsgai_pthread_mutex_destroy_checked(
            &s->mu, "pthread_mutex_destroy(nss_sync.mu cleanup)");
        return rc;
    }
    s->done   = 0;
    s->status = 0;
    s->res    = NULL;
    return 0;
}

static void
nss_sync_destroy(nss_sync_t *s)
{
    nsgai_pthread_mutex_destroy_checked(
        &s->mu, "pthread_mutex_destroy(nss_sync.mu)");
    nsgai_pthread_cond_destroy_checked(
        &s->cv, "pthread_cond_destroy(nss_sync.cv)");
}

/**
 * @brief Allocate aligned space from caller-provided NSS scratch buffer.
 */
static int
buf_alloc(char **bufp, size_t *remp, size_t need, size_t align, void **out)
{
    uintptr_t p       = (uintptr_t)*bufp;
    uintptr_t aligned = (p + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t pad        = (size_t)(aligned - p);
    if (need + pad > *remp) {
        return -1;
    }
    *out  = (void *)aligned;
    *bufp = (char *)aligned + need;
    *remp -= (need + pad);
    return 0;
}

/**
 * @brief Resolve a host name for a specific family using implicit netns.
 *
 * On failure, maps resolver status to NSS and host-db errno outputs.
 */
static enum nss_status
do_lookup(const char *name,
    int af,
    struct ares_addrinfo **out_ai,
    int *errnop,
    int *h_errnop)
{
    nsgai_ns_t ns;
    if (nsgai_ns_resolve_implicit(&ns) != 0) {
        *errnop   = errno;
        *h_errnop = NETDB_INTERNAL;
        return NSS_STATUS_UNAVAIL;
    }
    nsgai_channel_t *ch = NULL;
    int rc              = nsgai_channel_create(&ns, &ch);
    if (rc != ARES_SUCCESS) {
        nsgai_ns_release(&ns);
        *errnop   = ENOMEM;
        *h_errnop = NETDB_INTERNAL;
        return NSS_STATUS_UNAVAIL;
    }

    /* Synchronous wait */
    nss_sync_t s;
    rc = nss_sync_init(&s);
    if (rc != 0) {
        nsgai_channel_destroy(ch);
        nsgai_ns_release(&ns);
        *errnop   = rc;
        *h_errnop = NETDB_INTERNAL;
        return NSS_STATUS_UNAVAIL;
    }

    struct ares_addrinfo_hints hints = {0};
    hints.ai_family                  = af;

    ares_getaddrinfo(
        nsgai_channel_ares(ch), name, NULL, &hints, nsgai__nss_ai_cb, &s);

    nsgai_pthread_mutex_lock_or_abort(&s.mu, "pthread_mutex_lock(nss_sync.mu)");
    while (!s.done) {
        nsgai_pthread_cond_wait_or_abort(
            &s.cv, &s.mu, "pthread_cond_wait(nss_sync.cv)");
    }
    nsgai_pthread_mutex_unlock_or_abort(
        &s.mu, "pthread_mutex_unlock(nss_sync.mu)");

    nss_sync_destroy(&s);

    enum nss_status rv;
    if (s.status == ARES_SUCCESS && s.res && s.res->nodes) {
        *out_ai = s.res;
        rv      = NSS_STATUS_SUCCESS;
    } else {
        if (s.res) {
            ares_freeaddrinfo(s.res);
        }
        *out_ai = NULL;
        if (s.status == ARES_ENOTFOUND || s.status == ARES_ENODATA) {
            *errnop   = 0;
            *h_errnop = HOST_NOT_FOUND;
            rv        = NSS_STATUS_NOTFOUND;
        } else if (s.status == ARES_ETIMEOUT) {
            *errnop   = EAGAIN;
            *h_errnop = TRY_AGAIN;
            rv        = NSS_STATUS_TRYAGAIN;
        } else {
            *errnop   = EIO;
            *h_errnop = NO_RECOVERY;
            rv        = NSS_STATUS_UNAVAIL;
        }
    }

    nsgai_channel_destroy(ch);
    nsgai_ns_release(&ns);
    return rv;
}

static void
nsgai__nss_ai_cb(
    void *arg, int status, int timeouts, struct ares_addrinfo *result)
{
    (void)timeouts;
    nss_sync_t *s = (nss_sync_t *)arg;
    nsgai_pthread_mutex_lock_or_abort(
        &s->mu, "pthread_mutex_lock(nss_sync.mu)");
    s->status = status;
    s->res    = result;
    s->done   = 1;
    nsgai_pthread_cond_signal_or_abort(
        &s->cv, "pthread_cond_signal(nss_sync.cv)");
    nsgai_pthread_mutex_unlock_or_abort(
        &s->mu, "pthread_mutex_unlock(nss_sync.mu)");
}

/** @name NSS Entry Points */
/** @{ */

enum nss_status
_nss_nsgai_gethostbyname2_r(const char *name,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop)
{
    if (af != AF_INET && af != AF_INET6) {
        *errnop   = EAFNOSUPPORT;
        *h_errnop = NO_RECOVERY;
        return NSS_STATUS_UNAVAIL;
    }

    struct ares_addrinfo *ai = NULL;
    enum nss_status st       = do_lookup(name, af, &ai, errnop, h_errnop);
    if (st != NSS_STATUS_SUCCESS) {
        return st;
    }

    /* Layout in buffer:
     *  - h_name string
     *  - aliases NULL-terminated pointer array (we use no aliases)
     *  - addr_list NULL-terminated pointer array
     *  - addr blobs
     */
    char *bp    = buffer;
    size_t rem  = buflen;
    size_t alen = (af == AF_INET) ? 4 : 16;

    /* count nodes */
    size_t n = 0;
    for (struct ares_addrinfo_node *p = ai->nodes; p; p = p->ai_next) {
        if (p->ai_family == af) {
            n++;
        }
    }
    if (n == 0) {
        ares_freeaddrinfo(ai);
        *errnop   = 0;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    /* h_name */
    const char *canon = (ai->cnames && ai->cnames->name) ? ai->cnames->name
                        : ai->name                       ? ai->name
                                                         : name;
    void *p_name;
    size_t name_len = strlen(canon) + 1;
    if (buf_alloc(&bp, &rem, name_len, 1, &p_name) < 0) {
        goto erange;
    }
    memcpy(p_name, canon, name_len);

    /* aliases (empty array) */
    void *p_aliases;
    if (buf_alloc(&bp, &rem, sizeof(char *), sizeof(char *), &p_aliases) < 0) {
        goto erange;
    }
    ((char **)p_aliases)[0] = NULL;

    /* addr_list */
    void *p_addrlist;
    if (buf_alloc(&bp, &rem, sizeof(char *) * (n + 1), sizeof(char *),
            &p_addrlist) < 0) {
        goto erange;
    }

    /* addr blobs */
    size_t i = 0;
    for (struct ares_addrinfo_node *p = ai->nodes; p; p = p->ai_next) {
        if (p->ai_family != af) {
            continue;
        }
        void *blob;
        if (buf_alloc(&bp, &rem, alen, alen, &blob) < 0) {
            goto erange;
        }
        if (af == AF_INET) {
            memcpy(blob, &((struct sockaddr_in *)p->ai_addr)->sin_addr, alen);
        } else {
            memcpy(blob, &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr, alen);
        }
        ((char **)p_addrlist)[i++] = blob;
    }
    ((char **)p_addrlist)[i] = NULL;

    result->h_name      = (char *)p_name;
    result->h_aliases   = (char **)p_aliases;
    result->h_addrtype  = af;
    result->h_length    = (int)alen;
    result->h_addr_list = (char **)p_addrlist;

    ares_freeaddrinfo(ai);
    return NSS_STATUS_SUCCESS;

erange:
    ares_freeaddrinfo(ai);
    *errnop   = ERANGE;
    *h_errnop = NETDB_INTERNAL;
    return NSS_STATUS_TRYAGAIN;
}

enum nss_status
_nss_nsgai_gethostbyname_r(const char *name,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop)
{
    return _nss_nsgai_gethostbyname2_r(
        name, AF_INET, result, buffer, buflen, errnop, h_errnop);
}

/**
 * @brief Return merged IPv4/IPv6 answers as gaih_addrtuple chain.
 */

enum nss_status
_nss_nsgai_gethostbyname4_r(const char *name,
    struct gaih_addrtuple **pat,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop,
    int32_t *ttlp)
{
    if (ttlp) {
        *ttlp = 0;
    }

    struct ares_addrinfo *ai = NULL;
    enum nss_status st = do_lookup(name, AF_UNSPEC, &ai, errnop, h_errnop);
    if (st != NSS_STATUS_SUCCESS) {
        return st;
    }

    char *bp                    = buffer;
    size_t rem                  = buflen;
    struct gaih_addrtuple *prev = NULL;
    struct gaih_addrtuple *head = NULL;
    int32_t min_ttl             = INT32_MAX;
    int have_ttl                = 0;

    /* duplicate canonical name once */
    void *p_name;
    const char *canon = (ai->cnames && ai->cnames->name) ? ai->cnames->name
                        : ai->name                       ? ai->name
                                                         : name;
    size_t name_len   = strlen(canon) + 1;
    if (buf_alloc(&bp, &rem, name_len, 1, &p_name) < 0) {
        goto erange;
    }
    memcpy(p_name, canon, name_len);

    for (struct ares_addrinfo_node *p = ai->nodes; p; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) {
            continue;
        }
        void *node_p;
        if (buf_alloc(&bp, &rem, sizeof(struct gaih_addrtuple),
                _Alignof(struct gaih_addrtuple), &node_p) < 0) {
            goto erange;
        }
        struct gaih_addrtuple *t = (struct gaih_addrtuple *)node_p;
        memset(t, 0, sizeof(*t));
        t->name   = (char *)p_name;
        t->family = p->ai_family;
        if (p->ai_family == AF_INET) {
            memcpy(t->addr, &((struct sockaddr_in *)p->ai_addr)->sin_addr, 4);
        } else {
            memcpy(
                t->addr, &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr, 16);
            t->scopeid = ((struct sockaddr_in6 *)p->ai_addr)->sin6_scope_id;
        }
        if (!head) {
            head = t;
        }
        if (prev) {
            prev->next = t;
        }
        prev = t;

        if (p->ai_ttl > 0 && p->ai_ttl < min_ttl) {
            min_ttl  = p->ai_ttl;
            have_ttl = 1;
        }
    }

    if (!head) {
        ares_freeaddrinfo(ai);
        *errnop   = 0;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    if (ttlp && have_ttl) {
        *ttlp = min_ttl;
    }

    *pat = head;
    ares_freeaddrinfo(ai);
    return NSS_STATUS_SUCCESS;

erange:
    ares_freeaddrinfo(ai);
    *errnop   = ERANGE;
    *h_errnop = NETDB_INTERNAL;
    return NSS_STATUS_TRYAGAIN;
}

/**
 * @brief Reverse lookup entry point for hostent-based NSS API.
 */

enum nss_status
_nss_nsgai_gethostbyaddr_r(const void *addr,
    socklen_t len,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop)
{
    if ((af == AF_INET && len != 4) || (af == AF_INET6 && len != 16)) {
        *errnop   = EINVAL;
        *h_errnop = NETDB_INTERNAL;
        return NSS_STATUS_UNAVAIL;
    }

    struct sockaddr_storage ss;
    socklen_t slen;
    memset(&ss, 0, sizeof(ss));
    if (af == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        sin->sin_family         = AF_INET;
        memcpy(&sin->sin_addr, addr, 4);
        slen = sizeof(*sin);
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
        sin6->sin6_family         = AF_INET6;
        memcpy(&sin6->sin6_addr, addr, 16);
        slen = sizeof(*sin6);
    }

    char host[NI_MAXHOST];
    int rc = nsgai_getnameinfo(
        (struct sockaddr *)&ss, slen, host, sizeof(host), NULL, 0, NI_NAMEREQD);
    if (rc != 0) {
        if (rc == EAI_NONAME) {
            *errnop   = 0;
            *h_errnop = HOST_NOT_FOUND;
            return NSS_STATUS_NOTFOUND;
        }
        if (rc == EAI_AGAIN) {
            *errnop   = EAGAIN;
            *h_errnop = TRY_AGAIN;
            return NSS_STATUS_TRYAGAIN;
        }
        *errnop   = EIO;
        *h_errnop = NO_RECOVERY;
        return NSS_STATUS_UNAVAIL;
    }

    /* Pack result into buffer. */
    char *bp   = buffer;
    size_t rem = buflen;
    void *p_name, *p_aliases, *p_addrlist, *p_addr;
    size_t hlen = strlen(host) + 1;
    if (buf_alloc(&bp, &rem, hlen, 1, &p_name) < 0) {
        goto erange;
    }
    if (buf_alloc(&bp, &rem, sizeof(char *), sizeof(char *), &p_aliases) < 0) {
        goto erange;
    }
    if (buf_alloc(&bp, &rem, sizeof(char *) * 2, sizeof(char *), &p_addrlist) <
        0) {
        goto erange;
    }
    if (buf_alloc(&bp, &rem, len, len, &p_addr) < 0) {
        goto erange;
    }

    memcpy(p_name, host, hlen);
    ((char **)p_aliases)[0] = NULL;
    memcpy(p_addr, addr, len);
    ((char **)p_addrlist)[0] = p_addr;
    ((char **)p_addrlist)[1] = NULL;

    result->h_name      = (char *)p_name;
    result->h_aliases   = (char **)p_aliases;
    result->h_addrtype  = af;
    result->h_length    = (int)len;
    result->h_addr_list = (char **)p_addrlist;
    return NSS_STATUS_SUCCESS;

erange:
    *errnop   = ERANGE;
    *h_errnop = NETDB_INTERNAL;
    return NSS_STATUS_TRYAGAIN;
}

/**
 * @brief Reverse lookup variant that also reports TTL.
 *
 * Reverse TTL is currently unavailable, so @p ttlp is always set to 0.
 */

enum nss_status
_nss_nsgai_gethostbyaddr2_r(const void *addr,
    socklen_t len,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop,
    int32_t *ttlp)
{
    if (ttlp) {
        *ttlp = 0;
    }
    return _nss_nsgai_gethostbyaddr_r(
        addr, len, af, result, buffer, buflen, errnop, h_errnop);
}

/** @} */
