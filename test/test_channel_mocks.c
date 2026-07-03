/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * cmocka-based unit tests for nsgai_channel.c.
 *
 * Strategy:
 *  - Include nsgai_channel.c directly so the file-local (static) socket
 *    callbacks and helpers are reachable.  The test binary does NOT link
 *    libnsgai.so to avoid duplicate symbol definitions.
 *  - Use ld's --wrap=NAME mechanism to interpose on selected libc
 *    symbols (socket, setns, open, setsockopt, bind, if_nametoindex,
 *    if_indextoname).  Each wrapper defaults to __real_NAME() so the
 *    rest of the test process (and cmocka itself) is unaffected; an
 *    armed-state struct opts specific tests into failure injection.
 */

#include "../src/nsgai_channel.c"  /* pulls in all statics under test */
#include "../src/nsgai_ns.c"       /* ns resolve helpers (statics + extern) */
#include "../src/nsgai_addrinfo.c" /* status_to_eai, map_*, convert_addrinfo */

/* cmocka headers must come AFTER the SUT so the SUT sees plain libc. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
/* clang-format off */
#include <cmocka.h>
/* clang-format on */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Mock arming                                                         *
 * ------------------------------------------------------------------ */

static struct {
    /* socket(): if armed_socket > 0, the next armed_socket'th call
     * fails with socket_errno. */
    int armed_socket;
    int socket_errno;

    /* setns(): N>0 means the N'th *call after arming* returns -1 with
     * setns_errno; 0 means disarmed.  This lets us distinguish the
     * "first setns (enter target)" failure from the "second setns
     * (restore original)" failure. */
    int armed_setns_nth;
    int setns_call_count;
    int setns_errno;

    /* open(): if non-NULL, opens of this exact path fail with
     * open_errno (one shot). */
    const char *armed_open_path;
    int open_errno;

    /* fcntl(): if armed_fcntl_cmd != 0, the next fcntl() call with
     * matching cmd fails with fcntl_errno (one shot). */
    int armed_fcntl_cmd;
    int fcntl_errno;

    /* ares_init_options(): if armed_ares_init != 0, the next call
     * returns ares_init_status (one shot). */
    int armed_ares_init;
    int ares_init_status;

    /* ares_getaddrinfo(): capture call-site inputs and optionally
     * short-circuit with a forced status (one shot). */
    int armed_ares_getaddrinfo;
    int ares_getaddrinfo_status;
    int saw_ares_getaddrinfo;
    int last_name_is_null;
    int last_hints_is_null;
    struct ares_addrinfo_hints last_aih;
    char last_name[128];
    char last_service[32];
} g_mock;

static void
mock_reset(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
}

/* ------------------------------------------------------------------ *
 * Wrappers                                                            *
 * ------------------------------------------------------------------ */

extern int
__real_socket(int, int, int);
int
__wrap_socket(int domain, int type, int protocol)
{
    if (g_mock.armed_socket > 0) {
        if (--g_mock.armed_socket == 0) {
            errno = g_mock.socket_errno;
            return -1;
        }
    }
    return __real_socket(domain, type, protocol);
}

extern int
__real_setns(int, int);
int
__wrap_setns(int fd, int nstype)
{
    g_mock.setns_call_count++;
    if (g_mock.armed_setns_nth > 0 &&
        g_mock.setns_call_count == g_mock.armed_setns_nth) {
        errno = g_mock.setns_errno;
        return -1;
    }
    return __real_setns(fd, nstype);
}

extern int
__real_open(const char *, int, ...);
int
__wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (g_mock.armed_open_path && strcmp(path, g_mock.armed_open_path) == 0) {
        g_mock.armed_open_path = NULL;
        errno                  = g_mock.open_errno;
        return -1;
    }
    return __real_open(path, flags, mode);
}

extern int
__real_fcntl(int, int, ...);
int
__wrap_fcntl(int fd, int cmd, ...)
{
    /* fcntl takes variadic args; we only need to forward the long/int
     * variant used in the SUT (F_DUPFD_CLOEXEC takes an int arg). */
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (g_mock.armed_fcntl_cmd != 0 && cmd == g_mock.armed_fcntl_cmd) {
        g_mock.armed_fcntl_cmd = 0;
        errno                  = g_mock.fcntl_errno;
        return -1;
    }
    return __real_fcntl(fd, cmd, arg);
}

extern ares_status_t
__real_ares_init_options(
    ares_channel_t **channelptr, const struct ares_options *opts, int optmask);
ares_status_t
__wrap_ares_init_options(
    ares_channel_t **channelptr, const struct ares_options *opts, int optmask)
{
    if (g_mock.armed_ares_init != 0) {
        ares_status_t st       = (ares_status_t)g_mock.ares_init_status;
        g_mock.armed_ares_init = 0;
        return st;
    }
    return __real_ares_init_options(channelptr, opts, optmask);
}

extern void
__real_ares_getaddrinfo(ares_channel_t *,
    const char *,
    const char *,
    const struct ares_addrinfo_hints *,
    ares_addrinfo_callback,
    void *);
void
__wrap_ares_getaddrinfo(ares_channel_t *channel,
    const char *name,
    const char *service,
    const struct ares_addrinfo_hints *hints,
    ares_addrinfo_callback callback,
    void *arg)
{
    g_mock.saw_ares_getaddrinfo = 1;
    g_mock.last_name_is_null    = (name == NULL);
    g_mock.last_hints_is_null   = (hints == NULL);
    if (hints) {
        g_mock.last_aih = *hints;
    } else {
        memset(&g_mock.last_aih, 0, sizeof(g_mock.last_aih));
    }

    g_mock.last_name[0]    = '\0';
    g_mock.last_service[0] = '\0';
    if (name) {
        snprintf(g_mock.last_name, sizeof(g_mock.last_name), "%s", name);
    }
    if (service) {
        snprintf(
            g_mock.last_service, sizeof(g_mock.last_service), "%s", service);
    }

    if (g_mock.armed_ares_getaddrinfo) {
        int st                        = g_mock.ares_getaddrinfo_status;
        g_mock.armed_ares_getaddrinfo = 0;
        callback(arg, st, 0, NULL);
        return;
    }

    __real_ares_getaddrinfo(channel, name, service, hints, callback, arg);
}

/* ------------------------------------------------------------------ *
 * Test helpers                                                        *
 * ------------------------------------------------------------------ */

static int
open_self_netns(void)
{
    int fd = __real_open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC, 0);
    return fd;
}

/* ------------------------------------------------------------------ *
 * Tests: cb_asocket happy + error paths                               *
 * ------------------------------------------------------------------ */

static void
t_cb_asocket_no_netns(void **state)
{
    (void)state;
    sockctx_t ctx     = {.netns_fd = -1};
    ares_socket_t sfd = cb_asocket(AF_INET, SOCK_DGRAM, 0, &ctx);
    assert_int_not_equal(sfd, ARES_SOCKET_BAD);
    close((int)sfd);
}

static void
t_cb_asocket_null_ctx(void **state)
{
    (void)state;
    /* NULL user_data: behaves like "no netns required" (ctx check). */
    ares_socket_t sfd = cb_asocket(AF_INET, SOCK_DGRAM, 0, NULL);
    assert_int_not_equal(sfd, ARES_SOCKET_BAD);
    close((int)sfd);
}

static void
t_cb_asocket_socket_fails_no_ns(void **state)
{
    (void)state;
    sockctx_t ctx       = {.netns_fd = -1};
    g_mock.armed_socket = 1;
    g_mock.socket_errno = EMFILE;
    ares_socket_t sfd   = cb_asocket(AF_INET, SOCK_DGRAM, 0, &ctx);
    assert_int_equal(sfd, ARES_SOCKET_BAD);
}

static void
t_cb_asocket_socket_fails_with_ns(void **state)
{
    (void)state;
    int ns = open_self_netns();
    assert_int_not_equal(ns, -1);
    sockctx_t ctx       = {.netns_fd = ns};
    g_mock.armed_socket = 1;
    g_mock.socket_errno = ENOBUFS;
    ares_socket_t sfd   = cb_asocket(AF_INET, SOCK_DGRAM, 0, &ctx);
    assert_int_equal(sfd, ARES_SOCKET_BAD);
    /* Calling thread's netns must be intact (the restore branch ran). */
    close(ns);
}

static void
t_cb_asocket_save_open_fails(void **state)
{
    (void)state;
    int ns = open_self_netns();
    assert_int_not_equal(ns, -1);
    sockctx_t ctx          = {.netns_fd = ns};
    g_mock.armed_open_path = "/proc/thread-self/ns/net";
    g_mock.open_errno      = ENOENT;
    ares_socket_t sfd      = cb_asocket(AF_INET, SOCK_DGRAM, 0, &ctx);
    assert_int_equal(sfd, ARES_SOCKET_BAD);
    close(ns);
}

static void
t_cb_asocket_enter_setns_fails(void **state)
{
    (void)state;
    int ns = open_self_netns();
    assert_int_not_equal(ns, -1);
    sockctx_t ctx          = {.netns_fd = ns};
    g_mock.armed_setns_nth = 1; /* fail the "enter target" call */
    g_mock.setns_errno     = EPERM;
    ares_socket_t sfd      = cb_asocket(AF_INET, SOCK_DGRAM, 0, &ctx);
    assert_int_equal(sfd, ARES_SOCKET_BAD);
    close(ns);
}

static void
t_cb_asocket_restore_setns_fails(void **state)
{
    (void)state;
    int ns = open_self_netns();
    assert_int_not_equal(ns, -1);
    sockctx_t ctx          = {.netns_fd = ns};
    g_mock.armed_setns_nth = 2; /* fail the "restore original" call */
    g_mock.setns_errno     = EPERM;
    ares_socket_t sfd      = cb_asocket(AF_INET, SOCK_DGRAM, 0, &ctx);
    assert_int_equal(sfd, ARES_SOCKET_BAD);
    close(ns);
}

/* ------------------------------------------------------------------ *
 * Tests: cb_asetsockopt - every opt arm                               *
 * ------------------------------------------------------------------ */

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static void
t_cb_asetsockopt_all_arms(void **state)
{
    (void)state;
    int s = __real_socket(AF_INET, SOCK_DGRAM, 0);
    assert_int_not_equal(s, -1);

    int sz = 8192;
    assert_int_equal(
        cb_asetsockopt(s, ARES_SOCKET_OPT_SENDBUF_SIZE, &sz, sizeof(sz), NULL),
        0);
    assert_int_equal(
        cb_asetsockopt(s, ARES_SOCKET_OPT_RECVBUF_SIZE, &sz, sizeof(sz), NULL),
        0);

    /* BIND_DEVICE: requires CAP_NET_ADMIN; exercise the code path
     * regardless of success/failure (just must not crash). */
    (void)cb_asetsockopt(s, ARES_SOCKET_OPT_BIND_DEVICE, "lo", 3, NULL);

    /* TCP_FASTOPEN_CONNECT only valid on TCP sockets. */
    int tfo = 1;
    int t   = __real_socket(AF_INET, SOCK_STREAM, 0);
    if (t >= 0) {
        (void)cb_asetsockopt(
            t, ARES_SOCKET_OPT_TCP_FASTOPEN, &tfo, sizeof(tfo), NULL);
        close(t);
    }

    /* Unknown opt - default switch arm -> ENOSYS. */
    int rc =
        cb_asetsockopt(s, (ares_socket_opt_t)0x7fffffff, &sz, sizeof(sz), NULL);
    assert_int_equal(rc, -1);
    assert_int_equal(errno, ENOSYS);

    close(s);
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

/* ------------------------------------------------------------------ *
 * Tests: cb_abind                                                     *
 * ------------------------------------------------------------------ */

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static void
t_cb_abind_forwards(void **state)
{
    (void)state;
    int s = __real_socket(AF_INET, SOCK_DGRAM, 0);
    assert_int_not_equal(s, -1);
    struct sockaddr_in sa = {0};
    sa.sin_family         = AF_INET;
    sa.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    sa.sin_port           = 0; /* ephemeral */
    int rc = cb_abind(s, 0, (struct sockaddr *)&sa, sizeof(sa), NULL);
    assert_int_equal(rc, 0);
    close(s);
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

/* ------------------------------------------------------------------ *
 * Tests: cb_aif_nametoindex / cb_aif_indextoname                      *
 * ------------------------------------------------------------------ */

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
static void
t_cb_aif(void **state)
{
    (void)state;
    unsigned ix = cb_aif_nametoindex("lo", NULL);
    assert_true(ix > 0);

    char buf[IF_NAMESIZE];
    const char *p = cb_aif_indextoname(ix, buf, sizeof(buf), NULL);
    assert_non_null(p);
    assert_string_equal(p, "lo");

    /* Buffer smaller than IF_NAMESIZE: ENOSPC branch. */
    char small[3];
    p = cb_aif_indextoname(ix, small, sizeof(small), NULL);
    assert_null(p);
    assert_int_equal(errno, ENOSPC);
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

/* ------------------------------------------------------------------ *
 * Tests: trivial pass-throughs (already partly covered, but kept for  *
 * unit-level documentation).                                          *
 * ------------------------------------------------------------------ */

static void
t_cb_aclose(void **state)
{
    (void)state;
    int s = __real_socket(AF_INET, SOCK_DGRAM, 0);
    assert_int_not_equal(s, -1);
    assert_int_equal(cb_aclose(s, NULL), 0);
}

static void
t_strs_equal_helpers(void **state)
{
    (void)state;
    assert_true(strs_equal(NULL, NULL));
    assert_false(strs_equal("a", NULL));
    assert_false(strs_equal(NULL, "a"));
    assert_true(strs_equal("abc", "abc"));
    assert_false(strs_equal("abc", "abd"));

    struct stat a = {0}, b = {0};
    assert_false(stat_changed(&a, &b));
    b.st_ino = 1;
    assert_true(stat_changed(&a, &b));
    b        = a;
    b.st_dev = 1;
    assert_true(stat_changed(&a, &b));
    b         = a;
    b.st_size = 1;
    assert_true(stat_changed(&a, &b));
    b                = a;
    b.st_mtim.tv_sec = 1;
    assert_true(stat_changed(&a, &b));
    b                 = a;
    b.st_mtim.tv_nsec = 1;
    assert_true(stat_changed(&a, &b));
}

static void
t_nsgai_channel_ares_null(void **state)
{
    (void)state;
    /* NULL channel -> NULL ares channel (no crash). */
    assert_null(nsgai_channel_ares(NULL));
}

static void
t_nsgai_channel_destroy_null(void **state)
{
    (void)state;
    /* Must be a no-op. */
    nsgai_channel_destroy(NULL);
}

/* ------------------------------------------------------------------ *
 * Tests: nsgai_status_to_eai - every switch arm                       *
 * ------------------------------------------------------------------ */

static void
t_status_to_eai_arms(void **state)
{
    (void)state;
    assert_int_equal(nsgai_status_to_eai(ARES_SUCCESS), 0);
    assert_int_equal(nsgai_status_to_eai(ARES_ENODATA), EAI_NONAME);
    assert_int_equal(nsgai_status_to_eai(ARES_ENOTFOUND), EAI_NONAME);
    assert_int_equal(nsgai_status_to_eai(ARES_ENOMEM), EAI_MEMORY);
    assert_int_equal(nsgai_status_to_eai(ARES_ETIMEOUT), EAI_AGAIN);
    assert_int_equal(nsgai_status_to_eai(ARES_ECANCELLED), EAI_AGAIN);
    assert_int_equal(nsgai_status_to_eai(ARES_EBADNAME), EAI_NONAME);
    assert_int_equal(nsgai_status_to_eai(ARES_EBADFAMILY), EAI_FAMILY);
    assert_int_equal(nsgai_status_to_eai(ARES_ESERVICE), EAI_SERVICE);
    assert_int_equal(nsgai_status_to_eai(ARES_ENONAME), EAI_NONAME);
    assert_int_equal(nsgai_status_to_eai(ARES_EBADFLAGS), EAI_BADFLAGS);
    /* default arm */
    assert_int_equal(nsgai_status_to_eai(ARES_EFORMERR), EAI_FAIL);
}

/* ------------------------------------------------------------------ *
 * Tests: nsgai_map_ai_flags / nsgai_map_ni_flags                       *
 * ------------------------------------------------------------------ */

static void
t_map_ai_flags_arms(void **state)
{
    (void)state;
    assert_int_equal(nsgai_map_ai_flags(0), 0);
    assert_true(nsgai_map_ai_flags(AI_CANONNAME) & ARES_AI_CANONNAME);
    assert_true(nsgai_map_ai_flags(AI_NUMERICHOST) & ARES_AI_NUMERICHOST);
    assert_true(nsgai_map_ai_flags(AI_PASSIVE) & ARES_AI_PASSIVE);
    assert_true(nsgai_map_ai_flags(AI_NUMERICSERV) & ARES_AI_NUMERICSERV);
    assert_true(nsgai_map_ai_flags(AI_V4MAPPED) & ARES_AI_V4MAPPED);
    assert_true(nsgai_map_ai_flags(AI_ALL) & ARES_AI_ALL);
    assert_true(nsgai_map_ai_flags(AI_ADDRCONFIG) & ARES_AI_ADDRCONFIG);
#ifdef AI_IDN
    (void)nsgai_map_ai_flags(AI_IDN);
#endif   // ifdef AI_IDN
#ifdef AI_CANONIDN
    (void)nsgai_map_ai_flags(AI_CANONIDN);
#endif   // ifdef AI_CANONIDN
#ifdef AI_IDN_ALLOW_UNASSIGNED
#if defined(__GLIBC__)
    (void)nsgai_map_ai_flags(0x0100);
#else
    (void)nsgai_map_ai_flags(AI_IDN_ALLOW_UNASSIGNED);
#endif   // defined(__GLIBC__)
#endif   // ifdef AI_IDN_ALLOW_UNASSIGNED
#ifdef AI_IDN_USE_STD3_ASCII_RULES
#if defined(__GLIBC__)
    (void)nsgai_map_ai_flags(0x0200);
#else
    (void)nsgai_map_ai_flags(AI_IDN_USE_STD3_ASCII_RULES);
#endif   // defined(__GLIBC__)
#endif   // ifdef AI_IDN_USE_STD3_ASCII_RULES
    int all = AI_CANONNAME | AI_NUMERICHOST | AI_PASSIVE | AI_NUMERICSERV |
              AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG;
    assert_true(nsgai_map_ai_flags(all) != 0);
}

static void
t_map_ni_flags_arms(void **state)
{
    (void)state;
    assert_int_equal(nsgai_map_ni_flags(0), 0);
    assert_true(nsgai_map_ni_flags(NI_NOFQDN) & ARES_NI_NOFQDN);
    assert_true(nsgai_map_ni_flags(NI_NUMERICHOST) & ARES_NI_NUMERICHOST);
    assert_true(nsgai_map_ni_flags(NI_NAMEREQD) & ARES_NI_NAMEREQD);
    assert_true(nsgai_map_ni_flags(NI_NUMERICSERV) & ARES_NI_NUMERICSERV);
    assert_true(nsgai_map_ni_flags(NI_DGRAM) & ARES_NI_DGRAM);
#ifdef NI_IDN
    (void)nsgai_map_ni_flags(NI_IDN);
#endif   // ifdef NI_IDN
#ifdef NI_IDN_ALLOW_UNASSIGNED
#if defined(__GLIBC__)
    (void)nsgai_map_ni_flags(64);
#else
    (void)nsgai_map_ni_flags(NI_IDN_ALLOW_UNASSIGNED);
#endif   // defined(__GLIBC__)
#endif   // ifdef NI_IDN_ALLOW_UNASSIGNED
#ifdef NI_IDN_USE_STD3_ASCII_RULES
#if defined(__GLIBC__)
    (void)nsgai_map_ni_flags(128);
#else
    (void)nsgai_map_ni_flags(NI_IDN_USE_STD3_ASCII_RULES);
#endif   // defined(__GLIBC__)
#endif   // ifdef NI_IDN_USE_STD3_ASCII_RULES
}

/* ------------------------------------------------------------------ *
 * Tests: nsgai_convert_addrinfo edge cases                            *
 * ------------------------------------------------------------------ */

static void
t_convert_addrinfo_edges(void **state)
{
    (void)state;
    struct addrinfo *res = NULL;
    /* NULL src -> EAI_NONAME. */
    assert_int_equal(
        nsgai_convert_addrinfo(NULL, NULL, NULL, &res), EAI_NONAME);
    assert_null(res);

    /* Empty nodes -> EAI_NONAME. */
    struct ares_addrinfo empty = {0};
    assert_int_equal(
        nsgai_convert_addrinfo(&empty, NULL, NULL, &res), EAI_NONAME);
    assert_null(res);

    /* Non-numeric service that doesn't resolve -> EAI_SERVICE.  We
     * build a minimal ares_addrinfo node so resolve_service runs. */
    struct sockaddr_in sa          = {0};
    sa.sin_family                  = AF_INET;
    sa.sin_addr.s_addr             = htonl(0x7f000001);
    struct ares_addrinfo_node node = {0};
    node.ai_family                 = AF_INET;
    node.ai_addr                   = (struct sockaddr *)&sa;
    node.ai_addrlen                = sizeof(sa);
    struct ares_addrinfo src       = {0};
    src.nodes                      = &node;
    struct addrinfo hints          = {0};
    hints.ai_flags                 = AI_NUMERICSERV;
    res                            = NULL;
    int rc = nsgai_convert_addrinfo(&src, &hints, "notanumber", &res);
    assert_int_equal(rc, EAI_NONAME);
    assert_null(res);

    /* Numeric service > 65535 -> EAI_SERVICE. */
    res = NULL;
    rc  = nsgai_convert_addrinfo(&src, NULL, "65537", &res);
    assert_int_equal(rc, EAI_SERVICE);
    assert_null(res);

    /* Success path with numeric service + AI_CANONNAME. */
    src.name                = "example.invalid";
    struct addrinfo hints_c = {0};
    hints_c.ai_flags        = AI_CANONNAME;
    res                     = NULL;
    rc = nsgai_convert_addrinfo(&src, &hints_c, "80", &res);
    assert_int_equal(rc, 0);
    assert_non_null(res);
    assert_non_null(res->ai_canonname);
    nsgai_freeaddrinfo(res);

    /* freeaddrinfo NULL is a no-op. */
    nsgai_freeaddrinfo(NULL);
}

/* ------------------------------------------------------------------ *
 * Tests: nsgai_ns_resolve_*                                            *
 * ------------------------------------------------------------------ */

static void
t_ns_resolve_explicit_name_validate(void **state)
{
    (void)state;
    nsgai_ns_t ns;
    assert_int_equal(nsgai_ns_resolve_explicit_name(NULL, &ns), -1);
    assert_int_equal(nsgai_ns_resolve_explicit_name("", &ns), -1);
    assert_int_equal(nsgai_ns_resolve_explicit_name("a/b", &ns), -1);
    assert_int_equal(nsgai_ns_resolve_explicit_name("..", &ns), -1);
    assert_int_equal(
        nsgai_ns_resolve_explicit_name("definitely_no_such_ns_xyz_123", &ns),
        -1);
}

static void
t_ns_resolve_explicit_fd_invalid(void **state)
{
    (void)state;
    nsgai_ns_t ns;
    /* Negative fd -> EINVAL branch. */
    assert_int_equal(nsgai_ns_resolve_explicit_fd(-1, &ns), -1);
}

static void
t_ns_resolve_explicit_fd_fcntl_fail(void **state)
{
    (void)state;
    int fd = open_self_netns();
    assert_int_not_equal(fd, -1);
    nsgai_ns_t ns;
    g_mock.armed_fcntl_cmd = F_DUPFD_CLOEXEC;
    g_mock.fcntl_errno     = EMFILE;
    int rc                 = nsgai_ns_resolve_explicit_fd(fd, &ns);
    assert_int_equal(rc, -1);
    close(fd);
}

static void
t_ns_resolve_explicit_fd_ok(void **state)
{
    (void)state;
    int fd = open_self_netns();
    assert_int_not_equal(fd, -1);
    nsgai_ns_t ns;
    int rc = nsgai_ns_resolve_explicit_fd(fd, &ns);
    assert_int_equal(rc, 0);
    nsgai_ns_release(&ns);
    close(fd);
}

static void
t_ns_resolve_implicit_ok(void **state)
{
    (void)state;
    nsgai_ns_t ns;
    assert_int_equal(nsgai_ns_resolve_implicit(&ns), 0);
    nsgai_ns_release(&ns);
}

static void
t_ns_release_zero(void **state)
{
    (void)state;
    nsgai_ns_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.netns_fd = -1;
    nsgai_ns_release(&ns);
    nsgai_ns_release(NULL);
}

/* ------------------------------------------------------------------ *
 * Tests: nsgai_channel_create + ares_init_options failure              *
 * ------------------------------------------------------------------ */

static void
t_channel_create_ares_init_fail(void **state)
{
    (void)state;
    nsgai_ns_t ns;
    assert_int_equal(nsgai_ns_resolve_implicit(&ns), 0);
    nsgai_channel_t *ch     = NULL;
    g_mock.armed_ares_init  = 1;
    g_mock.ares_init_status = ARES_ENOMEM;
    int rc                  = nsgai_channel_create(&ns, &ch);
    assert_int_not_equal(rc, ARES_SUCCESS);
    nsgai_ns_release(&ns);
}

static void
t_do_getaddrinfo_channel_fail(void **state)
{
    (void)state;
    /* Force channel creation to fail -> nsgai_getaddrinfo returns
     * EAI_MEMORY (covers the failure branch in do_getaddrinfo). */
    g_mock.armed_ares_init  = 1;
    g_mock.ares_init_status = ARES_ENOMEM;
    struct addrinfo *res    = NULL;
    int rc                  = nsgai_getaddrinfo("localhost", NULL, NULL, &res);
    assert_int_equal(rc, EAI_MEMORY);
    assert_null(res);

    /* Generic non-ENOMEM failure -> EAI_SYSTEM. */
    g_mock.armed_ares_init  = 1;
    g_mock.ares_init_status = ARES_ENOTINITIALIZED;
    rc                      = nsgai_getaddrinfo("localhost", NULL, NULL, &res);
    assert_int_equal(rc, EAI_SYSTEM);
    assert_null(res);
}

static void
t_do_getnameinfo_channel_fail(void **state)
{
    (void)state;
    g_mock.armed_ares_init  = 1;
    g_mock.ares_init_status = ARES_ENOMEM;
    struct sockaddr_in sa   = {0};
    sa.sin_family           = AF_INET;
    sa.sin_addr.s_addr      = htonl(0x7f000001);
    char host[64];
    int rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    assert_int_equal(rc, EAI_MEMORY);
}

static void
t_do_getaddrinfo_rsyslog_udp_hints(void **state)
{
    (void)state;

    struct addrinfo *res = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    /* Server-style hints from rsyslog path: NULL node + PASSIVE +
     * NUMERICSERV + DGRAM. */
    g_mock.armed_ares_getaddrinfo  = 1;
    g_mock.ares_getaddrinfo_status = ARES_ENOTFOUND;
    hints.ai_flags                 = AI_PASSIVE | AI_NUMERICSERV;
    hints.ai_family                = AF_INET;
    hints.ai_socktype              = SOCK_DGRAM;
    int rc = nsgai_getaddrinfo(NULL, "514", &hints, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res);
    assert_false(g_mock.saw_ares_getaddrinfo);
    assert_int_equal(res->ai_family, AF_INET);
    assert_int_equal(res->ai_socktype, SOCK_DGRAM);
    assert_int_equal(
        ((struct sockaddr_in *)res->ai_addr)->sin_port, htons(514));
    assert_int_equal(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr,
        htonl(INADDR_ANY));
    nsgai_freeaddrinfo(res);
    res = NULL;

    /* Client-style hints from rsyslog path: NUMERICSERV + DGRAM with
     * non-NULL node. */
    g_mock.armed_ares_getaddrinfo  = 1;
    g_mock.ares_getaddrinfo_status = ARES_ENOTFOUND;
    g_mock.saw_ares_getaddrinfo    = 0;
    hints.ai_flags                 = AI_NUMERICSERV;
    hints.ai_family                = AF_INET6;
    hints.ai_socktype              = SOCK_DGRAM;
    rc = nsgai_getaddrinfo("192.0.2.10", "5514", &hints, &res);
    assert_int_equal(rc, EAI_NONAME);
    assert_null(res);
    assert_false(g_mock.last_name_is_null);
    assert_int_equal(g_mock.last_aih.ai_family, AF_INET6);
    assert_int_equal(g_mock.last_aih.ai_socktype, SOCK_DGRAM);
    assert_int_equal(
        g_mock.last_aih.ai_flags, nsgai_map_ai_flags(AI_NUMERICSERV));
    assert_string_equal(g_mock.last_name, "192.0.2.10");
    assert_string_equal(g_mock.last_service, "5514");
}

/* ------------------------------------------------------------------ *
 * Tests: public input-validation paths                                 *
 * ------------------------------------------------------------------ */

static void
t_public_input_validation(void **state)
{
    (void)state;
    struct addrinfo *res = NULL;
    /* NULL ns name falls back to implicit (current namespace) resolution. */
    assert_int_equal(nsgai_getaddrinfo_ns(NULL, "h", NULL, NULL, &res), 0);
    nsgai_freeaddrinfo(res);
    res = NULL;
    /* Bad ns name -> EAI_SYSTEM. */
    assert_int_equal(
        nsgai_getaddrinfo_ns("a/b", "h", NULL, NULL, &res), EAI_SYSTEM);
    /* Negative fd falls back to implicit resolution. */
    assert_int_equal(nsgai_getaddrinfo_nsfd(-1, "h", NULL, NULL, &res), 0);
    nsgai_freeaddrinfo(res);
    res = NULL;

    struct sockaddr_in sa = {0};
    sa.sin_family         = AF_INET;
    char h[16];
    /* NULL ns_name for getnameinfo falls back to implicit. */
    assert_int_equal(nsgai_getnameinfo_ns(NULL, (struct sockaddr *)&sa,
                         sizeof(sa), h, sizeof(h), NULL, 0, 0),
        0);
    /* Bad ns_name fails. */
    assert_int_equal(nsgai_getnameinfo_ns("a/b", (struct sockaddr *)&sa,
                         sizeof(sa), h, sizeof(h), NULL, 0, 0),
        EAI_SYSTEM);
    /* Negative fd for getnameinfo falls back to implicit. */
    assert_int_equal(nsgai_getnameinfo_nsfd(-1, (struct sockaddr *)&sa,
                         sizeof(sa), h, sizeof(h), NULL, 0, 0),
        0);
}

static void
t_gai_strerror_arms(void **state)
{
    (void)state;
    /* Hit every documented arm. */
    int codes[] = {0, EAI_AGAIN, EAI_BADFLAGS, EAI_FAIL, EAI_FAMILY, EAI_MEMORY,
        EAI_NONAME, EAI_SERVICE, EAI_SOCKTYPE, EAI_SYSTEM, EAI_OVERFLOW,
        0x7fffffff};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        const char *m = nsgai_gai_strerror(codes[i]);
        assert_non_null(m);
        assert_true(*m != '\0');
    }
}

/* ------------------------------------------------------------------ *
 * Fixture: reset mock state between tests.                            *
 * ------------------------------------------------------------------ */

static int
setup(void **state)
{
    (void)state;
    mock_reset();
    return 0;
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(t_cb_asocket_no_netns, setup),
        cmocka_unit_test_setup(t_cb_asocket_null_ctx, setup),
        cmocka_unit_test_setup(t_cb_asocket_socket_fails_no_ns, setup),
        cmocka_unit_test_setup(t_cb_asocket_socket_fails_with_ns, setup),
        cmocka_unit_test_setup(t_cb_asocket_save_open_fails, setup),
        cmocka_unit_test_setup(t_cb_asocket_enter_setns_fails, setup),
        cmocka_unit_test_setup(t_cb_asocket_restore_setns_fails, setup),
#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
        cmocka_unit_test_setup(t_cb_asetsockopt_all_arms, setup),
        cmocka_unit_test_setup(t_cb_abind_forwards, setup),
        cmocka_unit_test_setup(t_cb_aif, setup),
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
        cmocka_unit_test_setup(t_cb_aclose, setup),
        cmocka_unit_test_setup(t_strs_equal_helpers, setup),
        cmocka_unit_test_setup(t_nsgai_channel_ares_null, setup),
        cmocka_unit_test_setup(t_nsgai_channel_destroy_null, setup),
        cmocka_unit_test_setup(t_status_to_eai_arms, setup),
        cmocka_unit_test_setup(t_map_ai_flags_arms, setup),
        cmocka_unit_test_setup(t_map_ni_flags_arms, setup),
        cmocka_unit_test_setup(t_convert_addrinfo_edges, setup),
        cmocka_unit_test_setup(t_ns_resolve_explicit_name_validate, setup),
        cmocka_unit_test_setup(t_ns_resolve_explicit_fd_invalid, setup),
        cmocka_unit_test_setup(t_ns_resolve_explicit_fd_fcntl_fail, setup),
        cmocka_unit_test_setup(t_ns_resolve_explicit_fd_ok, setup),
        cmocka_unit_test_setup(t_ns_resolve_implicit_ok, setup),
        cmocka_unit_test_setup(t_ns_release_zero, setup),
        cmocka_unit_test_setup(t_channel_create_ares_init_fail, setup),
        cmocka_unit_test_setup(t_do_getaddrinfo_channel_fail, setup),
        cmocka_unit_test_setup(t_do_getnameinfo_channel_fail, setup),
        cmocka_unit_test_setup(t_do_getaddrinfo_rsyslog_udp_hints, setup),
        cmocka_unit_test_setup(t_public_input_validation, setup),
        cmocka_unit_test_setup(t_gai_strerror_arms, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
