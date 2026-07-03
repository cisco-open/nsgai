/**
 * @file nsgai.h
 * @brief Public API for network-namespace-aware name/address resolution.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * nsgai - network-namespace-aware getaddrinfo replacement built on c-ares.
 *
 * Design (post v0.2 simplification):
 *
 *   - Implicit entry points (nsgai_getaddrinfo, nsgai_getnameinfo,
 *     and the POSIX-named aliases) resolve in the *calling thread's
 *     current network namespace*, which we discover from
 *     /proc/thread-self/ns/net.  If that ns has a name (i.e. its inode
 *     matches an entry under /var/run/netns/), the per-ns
 *     /etc/netns/<name>/{resolv.conf,hosts} files are picked up
 *     automatically.  No setns() is performed - we're already there.
 *
 *   - Explicit entry points (_ns, _nsfd) target a *different* netns for
 *     one resolution.  The c-ares socket callback transiently does
 *     setns(CLONE_NEWNET) on its own event thread to create the UDP/
 *     TCP socket inside the target ns, then restores.  The calling
 *     thread's netns is never touched.  This is the multi-tenant case
 *     (one process serving multiple network namespaces, e.g. rsyslogd).
 *
 *   - There is intentionally no process-wide or thread-local "default
 *     namespace" override.  If you want libc resolution semantics, do
 *     not link or LD_PRELOAD this library.  If you want a single-ns
 *     application, run it under ip netns exec NAME (which bind-mounts
 *     /etc/netns/NAME/resolv.conf over /etc/resolv.conf for you) and
 *     use libc directly.
 */

#ifndef NSGAI_H
#define NSGAI_H

#include <netdb.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif   // ifdef __cplusplus

/**
 * @defgroup nsgai_implicit Implicit-namespace resolution
 * @brief Resolution in the calling thread's current network namespace.
 *
 * No setns() is performed.  The calling thread's current netns is
 * discovered from /proc/thread-self/ns/net, and any matching
 * /etc/netns/<name>/{resolv.conf,hosts} config is picked up automatically.
 */

/**
 * @defgroup nsgai_explicit_name Explicit resolution by namespace name
 * @brief Resolution in a named netns under /var/run/netns.
 *
 * The c-ares socket callback transiently performs setns(CLONE_NEWNET) on
 * its own event thread to create the UDP/TCP socket, then restores the
 * previous netns.  The calling thread is never touched.
 */

/**
 * @defgroup nsgai_explicit_fd Explicit resolution by namespace fd
 * @brief Resolution in a netns referenced by an open file descriptor.
 *
 * Same transient-setns mechanism as the @ref nsgai_explicit_name group;
 * the fd is dup'd internally so the caller may close it immediately.
 */

/**
 * @defgroup nsgai_posix POSIX-named weak aliases
 * @brief Weak POSIX symbols provided for preload / link-order interception.
 *
 * These symbols have default visibility and weak linkage so they intercept
 * libc when this library is loaded first, but can still be overridden.
 */

/** @brief Major version of the nsgai public API. */
#define NSGAI_VERSION_MAJOR 0
/** @brief Minor version of the nsgai public API. */
#define NSGAI_VERSION_MINOR 2
/** @brief Patch version of the nsgai public API. */
#define NSGAI_VERSION_PATCH 0

/** @addtogroup nsgai_implicit
 *  @{ */

/**
 * @brief Resolve node/service in the caller's current network namespace.
 *
 * Mirrors POSIX getaddrinfo() semantics, but resolves according to the
 * calling thread's current netns and any discovered /etc/netns/<name>/
 * resolver files.
 *
 * @param node Hostname or numeric address, or NULL.
 * @param service Service name or decimal port string, or NULL.
 * @param hints Optional selection hints; may be NULL.
 * @param res Output linked list on success; free with nsgai_freeaddrinfo().
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_getaddrinfo(const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res);

/**
 * @brief Reverse-resolve a socket address in the caller's current netns.
 *
 * Mirrors POSIX getnameinfo() semantics, with namespace-aware resolver
 * selection.
 *
 * @param sa Socket address to resolve.
 * @param salen Length of @p sa.
 * @param host Output host buffer (optional).
 * @param hostlen Length of @p host.
 * @param serv Output service buffer (optional).
 * @param servlen Length of @p serv.
 * @param flags NI_* flags.
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_getnameinfo(const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags);

/**
 * @brief Free an addrinfo list allocated by nsgai_getaddrinfo*().
 *
 * @param res Result list to free; NULL is allowed.
 */
void
nsgai_freeaddrinfo(struct addrinfo *res);

/**
 * @brief Convert EAI_* code to a static human-readable string.
 *
 * @param errcode Error code returned by nsgai_getaddrinfo*() or
 *        nsgai_getnameinfo*().
 * @return Non-NULL static string.
 */
const char *
nsgai_gai_strerror(int errcode);

/** @} */

/** @addtogroup nsgai_explicit_name
 *  @{ */

/**
 * @brief Resolve node/service in a named target network namespace.
 *
 * @param ns_name Namespace name (as found under /var/run/netns), or NULL
 *        to resolve in the calling thread's current namespace.
 * @param node Hostname or numeric address, or NULL.
 * @param service Service name or decimal port string, or NULL.
 * @param hints Optional selection hints; may be NULL.
 * @param res Output linked list on success; free with nsgai_freeaddrinfo().
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_getaddrinfo_ns(const char *ns_name,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res);

/**
 * @brief Reverse-resolve a socket address in a named target netns.
 *
 * @param ns_name Namespace name (as found under /var/run/netns), or NULL
 *        to resolve in the calling thread's current namespace.
 * @param sa Socket address to resolve.
 * @param salen Length of @p sa.
 * @param host Output host buffer (optional).
 * @param hostlen Length of @p host.
 * @param serv Output service buffer (optional).
 * @param servlen Length of @p serv.
 * @param flags NI_* flags.
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_getnameinfo_ns(const char *ns_name,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags);

/** @} */

/** @addtogroup nsgai_explicit_fd
 *  @{ */

/**
 * @brief Resolve node/service in a target netns referenced by fd.
 *
 * @param ns_fd Open netns fd (for example from /var/run/netns/<name> or
 *        /proc/<pid>/ns/net), or -1 to resolve in the calling thread's
 *        current namespace.
 * @param node Hostname or numeric address, or NULL.
 * @param service Service name or decimal port string, or NULL.
 * @param hints Optional selection hints; may be NULL.
 * @param res Output linked list on success; free with nsgai_freeaddrinfo().
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_getaddrinfo_nsfd(int ns_fd,
    const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res);

/**
 * @brief Reverse-resolve a socket address in a target netns by fd.
 *
 * @param ns_fd Open netns fd, or -1 to resolve in the calling thread's
 *        current namespace.
 * @param sa Socket address to resolve.
 * @param salen Length of @p sa.
 * @param host Output host buffer (optional).
 * @param hostlen Length of @p host.
 * @param serv Output service buffer (optional).
 * @param servlen Length of @p serv.
 * @param flags NI_* flags.
 * @return 0 on success, otherwise EAI_*.
 */
int
nsgai_getnameinfo_nsfd(int ns_fd,
    const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags);

/** @} */

/** @addtogroup nsgai_posix
 *  @{ */

/**
 * @brief POSIX getaddrinfo() alias forwarded by libnsgai.
 *
 * Mirrors nsgai_getaddrinfo() semantics when this library's weak symbol is
 * selected by link order or preload.
 */
int
getaddrinfo(const char *node,
    const char *service,
    const struct addrinfo *hints,
    struct addrinfo **res);

/** @brief POSIX freeaddrinfo() alias forwarded by libnsgai. */
void
freeaddrinfo(struct addrinfo *res);

/** @brief POSIX gai_strerror() alias forwarded by libnsgai. */
const char *
gai_strerror(int errcode);

/** @brief POSIX getnameinfo() alias forwarded by libnsgai. */
int
getnameinfo(const struct sockaddr *sa,
    socklen_t salen,
    char *host,
    socklen_t hostlen,
    char *serv,
    socklen_t servlen,
    int flags);

/** @} */

#ifdef __cplusplus
}
#endif   // ifdef __cplusplus

#endif   // ndef NSGAI_H
