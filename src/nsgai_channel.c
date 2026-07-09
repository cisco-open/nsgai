/**
 * @file nsgai_channel.c
 * @brief c-ares channel construction and cache keyed by network namespace.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Build a c-ares channel configured for a given network namespace.
 *
 * The socket-creation callback transiently enters the target netns
 * (per-thread setns(CLONE_NEWNET)) to create the UDP/TCP socket,
 * then immediately restores the previous netns.  The created socket
 * remains bound to the target netns for its lifetime.  All other
 * socket-related callbacks are thin wrappers around libc.
 */

#include "nsgai_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif   // ndef MSG_NOSIGNAL

/*
 * c-ares socket callback API compatibility:
 * - Newer c-ares: struct ares_socket_functions_ex + ares_set_socket_functions_ex
 * - Older c-ares: struct ares_socket_functions + ares_set_socket_functions
 */
/**
 * @brief Socket callback context stored with each cached channel.
 */
typedef struct {
    int netns_fd; /* -1 means "no setns required" */
} sockctx_t;

/**
 * @brief Cached and refcounted c-ares channel entry.
 *
 * Keyed by namespace identity plus optional per-namespace config paths.
 */
struct nsgai_channel {
    /* Cache key. */
    dev_t key_dev;
    ino_t key_ino;
    bool key_has_fd;       /* true => entry was built from explicit netns_fd */
    char *resolvconf_path; /* strdup of ns->resolvconf_path or NULL */
    char *hosts_path;      /* strdup of ns->hosts_path or NULL */

    /* Owned resources. */
    int netns_fd; /* dup'd from ns->netns_fd, or -1 (implicit) */
    ares_channel_t *channel;
    sockctx_t sockctx; /* address handed to c-ares as user_data */

    /* Freshness re-validation of resolv.conf / hosts. */
    struct stat resolv_st;
    struct stat hosts_st;
    bool resolv_valid;
    bool hosts_valid;

    /* Cache state. */
    int refcount;
    bool stale;
    bool cacheable;
    uint64_t lru_seq;
    struct nsgai_channel *next;
};

/** @name setns Helpers */
/** @{ */

static int
save_current_netns(void)
{
    /* /proc/thread-self ensures we capture *this* thread's netns. */
    return open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
}

static int
enter_netns(int fd)
{
    return setns(fd, CLONE_NEWNET);
}

/** @} */

/** @name Socket Callback Overrides */
/** @{ */

static ares_socket_t
cb_asocket(int domain, int type, int protocol, void *user_data)
{
    sockctx_t *ctx = (sockctx_t *)user_data;
    int saved_fd   = -1;
    int sock;
    int saved_errno = 0;

    if (ctx && ctx->netns_fd >= 0) {
        saved_fd = save_current_netns();
        if (saved_fd < 0) {
            return ARES_SOCKET_BAD;
        }
        if (enter_netns(ctx->netns_fd) != 0) {
            saved_errno = errno;
            close(saved_fd);
            errno = saved_errno;
            return ARES_SOCKET_BAD;
        }
    }

    sock        = socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    saved_errno = errno;

    if (saved_fd >= 0) {
        /* Best effort restore; if this fails the thread's view changes
         * which is a serious bug. */
        if (enter_netns(saved_fd) != 0) {
            int e = errno;
            close(saved_fd);
            errno = e;
            if (sock >= 0) {
                close(sock);
            }
            return ARES_SOCKET_BAD;
        }
        close(saved_fd);
    }

    errno = saved_errno;
    return (sock < 0) ? ARES_SOCKET_BAD : (ares_socket_t)sock;
}

static int
cb_aclose(ares_socket_t sock, void *user_data)
{
    (void)user_data;
    return close(sock);
}

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static int
cb_asetsockopt(ares_socket_t sock,
    ares_socket_opt_t opt,
    const void *val,
    ares_socklen_t val_size,
    void *user_data)
{
    (void)user_data;
    switch (opt) {
        case ARES_SOCKET_OPT_SENDBUF_SIZE:
            return setsockopt(sock, SOL_SOCKET, SO_SNDBUF, val, val_size);
        case ARES_SOCKET_OPT_RECVBUF_SIZE:
            return setsockopt(sock, SOL_SOCKET, SO_RCVBUF, val, val_size);
        case ARES_SOCKET_OPT_BIND_DEVICE:
#ifdef SO_BINDTODEVICE
            return setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, val, val_size);
#else    // ndef SO_BINDTODEVICE
            errno = ENOSYS;
            return -1;
#endif   // ndef SO_BINDTODEVICE
        case ARES_SOCKET_OPT_TCP_FASTOPEN:
#if defined(TCP_FASTOPEN_CONNECT)
            return setsockopt(
                sock, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, val, val_size);
#else    // ndef TCP_FASTOPEN_CONNECT
            errno = ENOSYS;
            return -1;
#endif   // ndef TCP_FASTOPEN_CONNECT
    }
    errno = ENOSYS;
    return -1;
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

static int
cb_aconnect(ares_socket_t sock,
    const struct sockaddr *address,
    ares_socklen_t address_len,
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    unsigned int flags,
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    void *user_data)
{
    (void)user_data;
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    (void)flags;
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    return connect(sock, address, address_len);
}

static ares_ssize_t
cb_arecvfrom(ares_socket_t sock,
    void *buffer,
    size_t length,
    int flags,
    struct sockaddr *address,
    ares_socklen_t *address_len,
    void *user_data)
{
    (void)user_data;
    return recvfrom(sock, buffer, length, flags, address, address_len);
}

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static ares_ssize_t
cb_asendto(ares_socket_t sock,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *address,
    ares_socklen_t address_len,
    void *user_data)
{
    (void)user_data;
    return sendto(
        sock, buffer, length, flags | MSG_NOSIGNAL, address, address_len);
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

#ifndef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static ares_ssize_t
cb_asendv(
    ares_socket_t sock, const struct iovec *data, int len, void *user_data)
{
    (void)user_data;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov    = (struct iovec *)data;
    msg.msg_iovlen = (size_t)len;
    return sendmsg(sock, &msg, MSG_NOSIGNAL);
}
#endif   // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static int
cb_agetsockname(ares_socket_t sock,
    struct sockaddr *address,
    ares_socklen_t *address_len,
    void *user_data)
{
    (void)user_data;
    return getsockname(sock, address, address_len);
}

static int
cb_abind(ares_socket_t sock,
    unsigned int flags,
    const struct sockaddr *address,
    socklen_t address_len,
    void *user_data)
{
    (void)user_data;
    (void)flags;
    return bind(sock, address, address_len);
}

static unsigned int
cb_aif_nametoindex(const char *ifname, void *user_data)
{
    (void)user_data;
    return if_nametoindex(ifname);
}

static const char *
cb_aif_indextoname(
    unsigned int ifindex, char *buf, size_t buflen, void *user_data)
{
    (void)user_data;
    if (buflen < IF_NAMESIZE) {
        errno = ENOSPC;
        return NULL;
    }
    return if_indextoname(ifindex, buf);
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

/** @} */

/**
 * @brief Socket callback vtable provided to c-ares channels.
 */
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static const struct ares_socket_functions_ex k_sockfns = {
    .version         = 1,
    .flags           = ARES_SOCKFUNC_FLAG_NONBLOCKING,
    .asocket         = cb_asocket,
    .aclose          = cb_aclose,
    .asetsockopt     = cb_asetsockopt,
    .aconnect        = cb_aconnect,
    .arecvfrom       = cb_arecvfrom,
    .asendto         = cb_asendto,
    .agetsockname    = cb_agetsockname,
    .abind           = cb_abind,
    .aif_nametoindex = cb_aif_nametoindex,
    .aif_indextoname = cb_aif_indextoname,
};
#else    // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static const struct ares_socket_functions k_sockfns = {
    .asocket   = cb_asocket,
    .aclose    = cb_aclose,
    .aconnect  = cb_aconnect,
    .arecvfrom = cb_arecvfrom,
    .asendv    = cb_asendv,
};
#endif   // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX

/**
 * @brief Per-namespace channel cache.
 *
 * Channel construction is expensive, so idle channels are retained and
 * revalidated against resolv.conf/hosts file metadata before reuse.
 */

#define NSGAI_CACHE_MAX_IDLE 32

static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static nsgai_channel_t *g_cache_head;
static int g_cache_idle;
static uint64_t g_lru_seq;

static bool
strs_equal(const char *a, const char *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static bool
stat_changed(const struct stat *old, const struct stat *cur)
{
    return old->st_ino != cur->st_ino || old->st_dev != cur->st_dev ||
           old->st_size != cur->st_size ||
           old->st_mtim.tv_sec != cur->st_mtim.tv_sec ||
           old->st_mtim.tv_nsec != cur->st_mtim.tv_nsec;
}

/**
 * @brief Compute namespace key tuple (dev, ino) for cache lookup.
 */
static int
compute_ns_key(const nsgai_ns_t *ns, dev_t *dev, ino_t *ino)
{
    struct stat st;
    if (ns && ns->netns_fd >= 0) {
        if (fstat(ns->netns_fd, &st) != 0) {
            return -1;
        }
    } else {
        if (stat("/proc/thread-self/ns/net", &st) != 0) {
            return -1;
        }
    }
    *dev = st.st_dev;
    *ino = st.st_ino;
    return 0;
}

/**
 * @brief Revalidate file-backed entry freshness.
 *
 * Returns true when tracked files have not changed since entry creation.
 */
static bool
entry_is_fresh(nsgai_channel_t *e)
{
    struct stat st;
    if (e->resolv_valid && e->resolvconf_path) {
        if (stat(e->resolvconf_path, &st) != 0) {
            return false;
        }
        if (stat_changed(&e->resolv_st, &st)) {
            return false;
        }
    }
    if (e->hosts_valid && e->hosts_path) {
        if (stat(e->hosts_path, &st) != 0) {
            return false;
        }
        if (stat_changed(&e->hosts_st, &st)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Free a channel entry detached from the cache list.
 */
static void
entry_free(nsgai_channel_t *e)
{
    if (!e) {
        return;
    }
    if (e->channel) {
        ares_destroy(e->channel);
    }
    if (e->netns_fd >= 0) {
        close(e->netns_fd);
    }
    free(e->resolvconf_path);
    free(e->hosts_path);
    free(e);
}

/**
 * @brief Unlink an entry from cache list. Caller holds cache lock.
 */
static void
cache_unlink(nsgai_channel_t *e)
{
    nsgai_channel_t **pp = &g_cache_head;
    while (*pp && *pp != e) {
        pp = &(*pp)->next;
    }
    if (*pp == e) {
        *pp = e->next;
    }
    e->next = NULL;
}

/**
 * @brief Find LRU idle entry. Caller holds cache lock.
 */
static nsgai_channel_t *
cache_find_lru_idle(void)
{
    nsgai_channel_t *best = NULL;
    for (nsgai_channel_t *e = g_cache_head; e; e = e->next) {
        if (e->refcount != 0) {
            continue;
        }
        if (!best || e->lru_seq < best->lru_seq) {
            best = e;
        }
    }
    return best;
}

/**
 * @brief Build and insert a fresh cache entry while lock is held.
 */
static int
channel_build_common(const nsgai_ns_t *ns,
    dev_t dev,
    ino_t ino,
    bool cacheable,
    nsgai_channel_t **out)
{
    nsgai_channel_t *e = calloc(1, sizeof(*e));
    if (!e) {
        return ARES_ENOMEM;
    }

    e->key_dev    = dev;
    e->key_ino    = ino;
    e->key_has_fd = (ns && ns->netns_fd >= 0);
    e->netns_fd   = -1;
    e->cacheable  = cacheable;

    if (ns && ns->netns_fd >= 0) {
        e->netns_fd = fcntl(ns->netns_fd, F_DUPFD_CLOEXEC, 0);
        if (e->netns_fd < 0) {
            free(e);
            return ARES_ENOMEM;
        }
    }
    e->sockctx.netns_fd = e->netns_fd;

    if (ns && ns->resolvconf_path) {
        e->resolvconf_path = strdup(ns->resolvconf_path);
        if (!e->resolvconf_path) {
            entry_free(e);
            return ARES_ENOMEM;
        }
        if (stat(e->resolvconf_path, &e->resolv_st) == 0) {
            e->resolv_valid = true;
        }
    }
    if (ns && ns->hosts_path) {
        e->hosts_path = strdup(ns->hosts_path);
        if (!e->hosts_path) {
            entry_free(e);
            return ARES_ENOMEM;
        }
        if (stat(e->hosts_path, &e->hosts_st) == 0) {
            e->hosts_valid = true;
        }
    }

    struct ares_options opts;
    int optmask = 0;
    memset(&opts, 0, sizeof(opts));
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    /* c-ares >= 1.34: ex socket callbacks coexist with event-thread mode. */
    opts.evsys = ARES_EVSYS_DEFAULT;
    optmask |= ARES_OPT_EVENT_THREAD;
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    if (e->resolvconf_path) {
        opts.resolvconf_path = e->resolvconf_path;
        optmask |= ARES_OPT_RESOLVCONF;
    }
    if (e->hosts_path) {
        opts.hosts_path = e->hosts_path;
        optmask |= ARES_OPT_HOSTS_FILE;
    }

    int rc = ares_init_options(&e->channel, &opts, optmask);
    if (rc != ARES_SUCCESS) {
        entry_free(e);
        return rc;
    }
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    if (ares_set_socket_functions_ex(e->channel, &k_sockfns, &e->sockctx) !=
        ARES_SUCCESS) {
        entry_free(e);
        return ARES_EFORMERR;
    }
#else    // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    ares_set_socket_functions(e->channel, &k_sockfns, &e->sockctx);
#endif   // ndef NSGAI_HAVE_CARES_SOCKFUNCS_EX

    e->refcount = 1;
    *out        = e;
    return ARES_SUCCESS;
}

static int
cache_build_locked(
    const nsgai_ns_t *ns, dev_t dev, ino_t ino, nsgai_channel_t **out)
{
    int rc = channel_build_common(ns, dev, ino, true, out);
    if (rc != ARES_SUCCESS) {
        return rc;
    }
    (*out)->next = g_cache_head;
    g_cache_head = *out;
    return ARES_SUCCESS;
}

/**
 * @brief Evict oldest idle entry when cache exceeds idle limit.
 */
static nsgai_channel_t *
cache_maybe_evict_locked(void)
{
    if (g_cache_idle <= NSGAI_CACHE_MAX_IDLE) {
        return NULL;
    }
    nsgai_channel_t *victim = cache_find_lru_idle();
    if (!victim) {
        return NULL;
    }
    cache_unlink(victim);
    g_cache_idle--;
    return victim;
}

int
nsgai_channel_create(const nsgai_ns_t *ns, nsgai_channel_t **out)
{
    dev_t dev;
    ino_t ino;
    if (compute_ns_key(ns, &dev, &ino) != 0) {
        return ARES_EFILE;
    }

    const bool has_fd       = (ns && ns->netns_fd >= 0);
    const char *want_resolv = ns ? ns->resolvconf_path : NULL;
    const char *want_hosts  = ns ? ns->hosts_path : NULL;
    nsgai_channel_t *victim = NULL;
    int rc                  = ARES_SUCCESS;

    nsgai_pthread_mutex_lock_or_abort(
        &g_cache_lock, "pthread_mutex_lock(g_cache_lock)");

    nsgai_channel_t *hit = NULL;
    for (nsgai_channel_t *e = g_cache_head; e; e = e->next) {
        if (e->stale) {
            continue;
        }
        if (e->key_dev != dev || e->key_ino != ino) {
            continue;
        }
        if (e->key_has_fd != has_fd) {
            continue;
        }
        if (!strs_equal(e->resolvconf_path, want_resolv)) {
            continue;
        }
        if (!strs_equal(e->hosts_path, want_hosts)) {
            continue;
        }
        if (!entry_is_fresh(e)) {
            e->stale = true;
            /* If nobody is using it, drop now; else owners will drop on
             * release. */
            if (e->refcount == 0) {
                cache_unlink(e);
                g_cache_idle--;
                victim = e; /* destroy after unlock */
            }
            continue;
        }
        hit = e;
        break;
    }

    if (hit) {
        if (hit->refcount == 0) {
            g_cache_idle--;
        }
        hit->refcount++;
        *out = hit;
    } else {
        rc = cache_build_locked(ns, dev, ino, out);
    }

    nsgai_pthread_mutex_unlock_or_abort(
        &g_cache_lock, "pthread_mutex_unlock(g_cache_lock)");

    if (victim) {
        entry_free(victim);
    }
    return rc;
}

int
nsgai_channel_create_uncached(const nsgai_ns_t *ns, nsgai_channel_t **out)
{
    dev_t dev;
    ino_t ino;

    if (compute_ns_key(ns, &dev, &ino) != 0) {
        return ARES_EFILE;
    }

    return channel_build_common(ns, dev, ino, false, out);
}

void
nsgai_channel_destroy(nsgai_channel_t *ch)
{
    if (!ch) {
        return;
    }

    nsgai_channel_t *to_free = NULL;
    nsgai_channel_t *evicted = NULL;

    nsgai_pthread_mutex_lock_or_abort(
        &g_cache_lock, "pthread_mutex_lock(g_cache_lock)");
    if (!ch->cacheable) {
        nsgai_pthread_mutex_unlock_or_abort(
            &g_cache_lock, "pthread_mutex_unlock(g_cache_lock)");
        entry_free(ch);
        return;
    }
    if (--ch->refcount == 0) {
        if (ch->stale) {
            cache_unlink(ch);
            to_free = ch;
        } else {
            ch->lru_seq = ++g_lru_seq;
            g_cache_idle++;
            evicted = cache_maybe_evict_locked();
        }
    }
    nsgai_pthread_mutex_unlock_or_abort(
        &g_cache_lock, "pthread_mutex_unlock(g_cache_lock)");

    if (to_free) {
        entry_free(to_free);
    }
    if (evicted) {
        entry_free(evicted);
    }
}

ares_channel_t *
nsgai_channel_ares(nsgai_channel_t *ch)
{
    return ch ? ch->channel : NULL;
}
