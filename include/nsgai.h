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
#include <signal.h>
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

/**
 * @brief Host/DNS lookup ordering policy for namespace-aware resolution.
 *
 * These policies apply to both forward and reverse host lookups. Service-name
 * resolution remains process-global and follows libc semantics.
 */
typedef enum {
    NSGAI_LOOKUP_HOSTS_FIRST = 0,
    NSGAI_LOOKUP_DNS_FIRST   = 1,
    NSGAI_LOOKUP_HOSTS_ONLY  = 2,
    NSGAI_LOOKUP_DNS_ONLY    = 3,
} nsgai_lookup_policy_t;

/**
 * @brief Set the process-wide host/DNS lookup policy used by libnsgai.
 *
 * This policy applies to subsequent synchronous and asynchronous lookups in
 * the current process. Callers are expected to set it during process startup
 * before concurrent resolution begins.
 *
 * @param policy Lookup ordering policy.
 * @return 0 on success, otherwise EAI_BADFLAGS for an unsupported value.
 */
int
nsgai_set_lookup_policy(nsgai_lookup_policy_t policy);

#ifdef __USE_GNU
/**
 * @defgroup nsgai_async GNU-style asynchronous getaddrinfo
 * @brief Async lookup API compatible with glibc's gaicb request structure.
 *
 * These functions mirror the GNU getaddrinfo_a/gai_* family. Namespace-aware
 * variants follow the same suffix pattern as the synchronous API.
 *
 * Completed request status is stored in the caller-provided struct gaicb, so
 * callers can use the same request object shape regardless of whether they
 * select libc or nsgai.
 */
#endif

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

/**
 * @brief Release process-global resources retained by libnsgai.
 *
 * Synchronous lookups may keep idle c-ares channels cached for reuse. Calling
 * this function destroys those idle channels immediately. It is also invoked
 * automatically when libnsgai is unloaded or the process exits.
 *
 * Call this only when no other thread is concurrently using libnsgai.
 */
void
nsgai_cleanup(void);

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

#ifdef __USE_GNU
/** @addtogroup nsgai_async
 *  @{ */

/**
 * @brief Enqueue or synchronously execute one or more getaddrinfo requests.
 *
 * The request objects use the GNU struct gaicb layout and resolve in the
 * calling thread's current network namespace.
 *
 * @param mode GAI_WAIT or GAI_NOWAIT.
 * @param list Array of gaicb request pointers.
 * @param ent Number of entries in @p list.
 * @param sig Optional asynchronous completion notification for GAI_NOWAIT.
 * @return 0 if requests were accepted, otherwise EAI_* setup error.
 */
int
nsgai_getaddrinfo_a(int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig);

/**
 * @brief Enqueue or synchronously execute requests in a named target netns.
 *
 * @param ns_name Namespace name under /var/run/netns, or NULL to resolve in
 *        the calling thread's current network namespace.
 * @param mode GAI_WAIT or GAI_NOWAIT.
 * @param list Array of gaicb request pointers.
 * @param ent Number of entries in @p list.
 * @param sig Optional asynchronous completion notification for GAI_NOWAIT.
 * @return 0 if requests were accepted, otherwise EAI_* setup error.
 */
int
nsgai_getaddrinfo_a_ns(const char *ns_name,
    int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig);

/**
 * @brief Enqueue or synchronously execute requests in a target netns by fd.
 *
 * @param ns_fd Open netns fd, or a negative value to resolve in the current
 *        thread's network namespace.
 * @param mode GAI_WAIT or GAI_NOWAIT.
 * @param list Array of gaicb request pointers.
 * @param ent Number of entries in @p list.
 * @param sig Optional asynchronous completion notification for GAI_NOWAIT.
 * @return 0 if requests were accepted, otherwise EAI_* setup error.
 */
int
nsgai_getaddrinfo_a_nsfd(int ns_fd,
    int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig);

/**
 * @brief Wait until at least one listed request completes.
 *
 * @param list Array of gaicb request pointers.
 * @param ent Number of entries in @p list.
 * @param timeout Optional relative timeout.
 * @return 0 if at least one request completed, else EAI_*.
 */
int
nsgai_suspend(
    const struct gaicb *const list[], int ent, const struct timespec *timeout);

/**
 * @brief Query status of an asynchronous request.
 *
 * @param req Request to query.
 * @return EAI_INPROGRESS, 0, or an error code.
 */
int
nsgai_error(struct gaicb *req);

/**
 * @brief Attempt to cancel one or more asynchronous requests.
 *
 * @param req Request to cancel, or NULL to cancel all outstanding requests.
 * @return EAI_CANCELED, EAI_NOTCANCELED, or EAI_ALLDONE.
 */
int
nsgai_cancel(struct gaicb *req);

/** @} */
#endif

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

#ifdef __USE_GNU
/** @brief GNU getaddrinfo_a() alias routed to nsgai in the current namespace. */
int
getaddrinfo_a(int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig);

/** @brief GNU gai_suspend() alias routed to nsgai. */
int
gai_suspend(
    const struct gaicb *const list[], int ent, const struct timespec *timeout);

/** @brief GNU gai_error() alias routed to nsgai. */
int
gai_error(struct gaicb *req);

/** @brief GNU gai_cancel() alias routed to nsgai. */
int
gai_cancel(struct gaicb *req);
#endif

/** @} */

#ifdef __cplusplus
}
#endif   // ifdef __cplusplus

#endif   // ndef NSGAI_H
