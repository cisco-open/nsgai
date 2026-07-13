/**
 * @file nsgai_internal.h
 * @brief Internal interfaces shared between nsgai implementation units.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 */
#ifndef NSGAI_INTERNAL_H
#define NSGAI_INTERNAL_H

#include "nsgai.h"
#include <ares.h>
#include <ares_version.h>
#include <pthread.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef ARES_VERSION
#define ARES_VERSION 0
#endif   // ndef ARES_VERSION

#if ARES_VERSION >= ((1 << 16) | (34 << 8) | 0)
#define NSGAI_HAVE_CARES_SOCKFUNCS_EX 1
#endif   // ARES_VERSION >= 1.34.0

#ifdef __cplusplus
extern "C" {
#endif   // ifdef __cplusplus

#define NSGAI_MAX_CHANNEL_SOCKETS 64

/**
 * @brief Resolved namespace selection consumed by core resolution paths.
 *
 * Semantics:
 * - @c netns_fd == -1: do not setns(); resolve in current thread netns.
 * - @c netns_fd >= 0: create sockets in that netns via transient setns().
 * - @c resolvconf_path/@c hosts_path: optional per-namespace config files.
 */
typedef struct {
    int netns_fd;
    bool owns_netns_fd; /* if true, nsgai_ns_release closes it */
    char *resolvconf_path;
    char *hosts_path;
} nsgai_ns_t;

/**
 * @brief Resolve explicit namespace by name.
 *
 * @param ns_name Namespace name under /var/run/netns.
 * @param out Output namespace descriptor.
 * @return 0 on success, -1 on failure with errno set.
 */
int
nsgai_ns_resolve_explicit_name(const char *ns_name, nsgai_ns_t *out);

/**
 * @brief Resolve explicit namespace from a caller-provided netns fd.
 *
 * @param ns_fd Netns fd to duplicate and own in @p out.
 * @param out Output namespace descriptor.
 * @return 0 on success, -1 on failure with errno set.
 */
int
nsgai_ns_resolve_explicit_fd(int ns_fd, nsgai_ns_t *out);

/**
 * @brief Resolve implicit namespace using the current thread's netns.
 *
 * @param out Output namespace descriptor.
 * @return 0 on success, -1 on failure with errno set.
 */
int
nsgai_ns_resolve_implicit(nsgai_ns_t *out);

/**
 * @brief Release resources held by a namespace descriptor.
 *
 * Safe on zero-initialized instances.
 *
 * @param ns Descriptor to release.
 */
void
nsgai_ns_release(nsgai_ns_t *ns);

/**
 * @brief Opaque cached resolver-channel handle.
 */
typedef struct nsgai_channel nsgai_channel_t;

/**
 * @brief Acquire or build a channel configured for @p ns.
 *
 * @param ns Namespace selection descriptor.
 * @param out Output channel handle.
 * @return c-ares status code.
 */
int
nsgai_channel_create(const nsgai_ns_t *ns, nsgai_channel_t **out);

/**
 * @brief Build a dedicated uncached channel configured for @p ns.
 *
 * This is intended for async request state that must not share channel
 * ownership or polling responsibilities with the cached synchronous path.
 *
 * @param ns Namespace selection descriptor.
 * @param out Output channel handle.
 * @return c-ares status code.
 */
int
nsgai_channel_create_uncached(const nsgai_ns_t *ns, nsgai_channel_t **out);

/**
 * @brief Release a channel handle acquired via nsgai_channel_create().
 *
 * @param ch Channel handle (NULL allowed).
 */
void
nsgai_channel_destroy(nsgai_channel_t *ch);

/**
 * @brief Destroy all idle cached resolver channels.
 *
 * This is called during library teardown so c-ares event threads owned by
 * cached synchronous channels stop before libnsgai's callback code unloads.
 * Channels that are still referenced are marked stale and destroyed by their
 * final nsgai_channel_destroy().
 */
void
nsgai_channel_cache_cleanup(void);

/**
 * @brief Access underlying c-ares channel pointer.
 *
 * @param ch Channel handle.
 * @return Underlying ares_channel_t or NULL when @p ch is NULL.
 */
ares_channel_t *
nsgai_channel_ares(nsgai_channel_t *ch);

/**
 * @brief Snapshot active channel sockets as pollfd entries.
 *
 * @param ch Channel handle.
 * @param pfds Caller-provided pollfd array.
 * @param maxfds Number of entries in @p pfds.
 * @return Number of populated pollfd entries.
 */
nfds_t
nsgai_channel_pollfds(nsgai_channel_t *ch, struct pollfd *pfds, nfds_t maxfds);

/**
 * @brief Feed poll results back into c-ares for a channel.
 *
 * @param ch Channel handle.
 * @param pfds pollfd array previously populated by nsgai_channel_pollfds().
 * @param nfds Number of entries in @p pfds.
 */
void
nsgai_channel_process_pollfds(
    nsgai_channel_t *ch, const struct pollfd *pfds, nfds_t nfds);

/**
 * @brief Internal strerror helper for EAI_* values.
 *
 * @param code EAI_* code.
 * @return Static error string.
 */
const char *
nsgai_gai_strerror_impl(int code);

/**
 * @brief Map c-ares status to EAI_* status.
 *
 * @param s c-ares status code.
 * @return Corresponding EAI_* code.
 */
int
nsgai_status_to_eai(ares_status_t s);

/**
 * @brief Translate libc AI_* bitmask to ARES_AI_*.
 *
 * @param libc_flags libc AI_* bitmask.
 * @return Equivalent c-ares AI bitmask.
 */
int
nsgai_map_ai_flags(int libc_flags);

/**
 * @brief Translate libc NI_* bitmask to ARES_NI_*.
 *
 * @param libc_flags libc NI_* bitmask.
 * @return Equivalent c-ares NI bitmask.
 */
int
nsgai_map_ni_flags(int libc_flags);

/**
 * @brief Convert c-ares result chain to POSIX addrinfo chain.
 *
 * @param src c-ares result structure.
 * @param hints Optional caller hints.
 * @param service Service string used to derive port where applicable.
 * @param out Converted addrinfo chain on success.
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_convert_addrinfo(const struct ares_addrinfo *src,
    const struct addrinfo *hints,
    const char *service,
    struct addrinfo **out);

/**
 * @brief Lock mutex or abort on failure after logging.
 * @param mu Mutex to lock.
 * @param what Context string for diagnostics.
 */
void
nsgai_pthread_mutex_lock_or_abort(pthread_mutex_t *mu, const char *what);

/**
 * @brief Unlock mutex or abort on failure after logging.
 * @param mu Mutex to unlock.
 * @param what Context string for diagnostics.
 */
void
nsgai_pthread_mutex_unlock_or_abort(pthread_mutex_t *mu, const char *what);

/**
 * @brief Wait on condition variable or abort on failure after logging.
 * @param cv Condition variable.
 * @param mu Associated mutex.
 * @param what Context string for diagnostics.
 */
void
nsgai_pthread_cond_wait_or_abort(
    pthread_cond_t *cv, pthread_mutex_t *mu, const char *what);

/**
 * @brief Signal condition variable or abort on failure after logging.
 * @param cv Condition variable.
 * @param what Context string for diagnostics.
 */
void
nsgai_pthread_cond_signal_or_abort(pthread_cond_t *cv, const char *what);

/**
 * @brief Initialize mutex and log failure to syslog.
 * @param mu Mutex to initialize.
 * @param what Context string for diagnostics.
 * @return pthread return code.
 */
int
nsgai_pthread_mutex_init_checked(pthread_mutex_t *mu, const char *what);

/**
 * @brief Initialize condition variable and log failure to syslog.
 * @param cv Condition variable to initialize.
 * @param what Context string for diagnostics.
 * @return pthread return code.
 */
int
nsgai_pthread_cond_init_checked(pthread_cond_t *cv, const char *what);

/**
 * @brief Destroy mutex and log (but do not abort) on failure.
 * @param mu Mutex to destroy.
 * @param what Context string for diagnostics.
 */
void
nsgai_pthread_mutex_destroy_checked(pthread_mutex_t *mu, const char *what);

/**
 * @brief Destroy condition variable and log (but do not abort) on failure.
 * @param cv Condition variable to destroy.
 * @param what Context string for diagnostics.
 */
void
nsgai_pthread_cond_destroy_checked(pthread_cond_t *cv, const char *what);

#ifdef __cplusplus
}
#endif   // ifdef __cplusplus

#endif   // ndef NSGAI_INTERNAL_H
