/**
 * @file preload.c
 * @brief LD_PRELOAD shim that redirects POSIX resolver calls to nsgai.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * libnsgai_preload.so - non-transparent LD_PRELOAD shim.
 *
 * When loaded ahead of libc, every call to the POSIX name-resolution
 * functions is redirected to the matching nsgai_* implementation.
 * There is no fall-through to libc and no per-call gating: if this
 * library is preloaded, the process is opting in to nsgai semantics
 * for *all* DNS / hosts lookups.  Per-thread netns selection is
 * implicit (current thread's network namespace; see nsgai.h).
 *
 * If you need only some calls to go through nsgai, do not preload -
 * link or dlopen libnsgai.so and call the nsgai_* / nsgai_*_ns()
 * entry points directly.
 */

#include "nsgai.h"

#include <stddef.h>

/**
 * @brief POSIX getaddrinfo alias routed to nsgai implementation.
 */
int
getaddrinfo(const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res)
{
    return nsgai_getaddrinfo(node, service, hints, res);
}

/**
 * @brief POSIX freeaddrinfo alias routed to nsgai implementation.
 */
void
freeaddrinfo(struct addrinfo *res)
{
    nsgai_freeaddrinfo(res);
}

/**
 * @brief POSIX gai_strerror alias routed to nsgai implementation.
 */
const char *
gai_strerror(int errcode)
{
    return nsgai_gai_strerror(errcode);
}

/**
 * @brief POSIX getnameinfo alias routed to nsgai implementation.
 */
int
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

#ifdef __USE_GNU
int
getaddrinfo_a(
    int mode, struct gaicb *list[__restrict_arr], int ent, struct sigevent *sig)
{
    return nsgai_getaddrinfo_a(mode, list, ent, sig);
}

int
gai_suspend(
    const struct gaicb *const list[], int ent, const struct timespec *timeout)
{
    return nsgai_suspend(list, ent, timeout);
}

int
gai_error(struct gaicb *req)
{
    return nsgai_error(req);
}

int
gai_cancel(struct gaicb *req)
{
    return nsgai_cancel(req);
}
#endif
