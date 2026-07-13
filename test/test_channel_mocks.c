/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
#include <sys/wait.h>
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

    int calloc_call_count;
    int calloc_fail_nth;

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

    int armed_ares_set_socket_functions_ex;
    int ares_set_socket_functions_ex_status;

    int armed_pthread_mutex_lock;
    int pthread_mutex_lock_rc;

    int armed_pthread_mutex_unlock;
    int pthread_mutex_unlock_rc;

    int armed_pthread_cond_wait;
    int pthread_cond_wait_rc;

    int armed_pthread_cond_signal;
    int pthread_cond_signal_rc;

    int armed_pthread_mutex_init;
    int pthread_mutex_init_rc;

    int armed_pthread_cond_init;
    int pthread_cond_init_rc;

    int armed_pthread_mutex_destroy;
    int pthread_mutex_destroy_rc;

    int armed_pthread_cond_destroy;
    int pthread_cond_destroy_rc;

    int abort_exit_code;
} g_mock;

static jmp_buf g_abort_jmp;
static int g_abort_expect = 0;

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

static int
nsgai_test_forward_open(int (*real_open_fn)(const char *, int, ...),
    const char *path,
    int flags,
    va_list ap)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(ap, mode_t);
    }
    if (g_mock.armed_open_path && strcmp(path, g_mock.armed_open_path) == 0) {
        g_mock.armed_open_path = NULL;
        errno                  = g_mock.open_errno;
        return -1;
    }
    return real_open_fn(path, flags, mode);
}

int
__wrap_open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int rc = nsgai_test_forward_open(__real_open, path, flags, ap);
    va_end(ap);
    return rc;
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

extern int
__real_open64(const char *, int, ...);
int
__wrap_open64(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int rc = nsgai_test_forward_open(__real_open64, path, flags, ap);
    va_end(ap);
    return rc;
}

extern int
__real___open_2(const char *, int);
int
__wrap___open_2(const char *path, int flags)
{
    if (g_mock.armed_open_path && strcmp(path, g_mock.armed_open_path) == 0) {
        g_mock.armed_open_path = NULL;
        errno                  = g_mock.open_errno;
        return -1;
    }
    return __real___open_2(path, flags);
}

extern void *
__real_calloc(size_t, size_t);
void *
__wrap_calloc(size_t nmemb, size_t size)
{
    g_mock.calloc_call_count++;
    if (g_mock.calloc_fail_nth > 0 &&
        g_mock.calloc_call_count == g_mock.calloc_fail_nth) {
        return NULL;
    }
    return __real_calloc(nmemb, size);
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

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
extern ares_status_t
__real_ares_set_socket_functions_ex(ares_channel_t *channel,
    const struct ares_socket_functions_ex *socket_functions,
    void *user_data);
ares_status_t
__wrap_ares_set_socket_functions_ex(ares_channel_t *channel,
    const struct ares_socket_functions_ex *socket_functions,
    void *user_data)
{
    if (g_mock.armed_ares_set_socket_functions_ex) {
        g_mock.armed_ares_set_socket_functions_ex = 0;
        return (ares_status_t)g_mock.ares_set_socket_functions_ex_status;
    }
    return __real_ares_set_socket_functions_ex(
        channel, socket_functions, user_data);
}
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

extern int
__real_pthread_mutex_lock(pthread_mutex_t *);
int
__wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
    if (g_mock.armed_pthread_mutex_lock) {
        g_mock.armed_pthread_mutex_lock = 0;
        return g_mock.pthread_mutex_lock_rc;
    }
    return __real_pthread_mutex_lock(mu);
}

extern int
__real_pthread_mutex_unlock(pthread_mutex_t *);
int
__wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
    if (g_mock.armed_pthread_mutex_unlock) {
        g_mock.armed_pthread_mutex_unlock = 0;
        return g_mock.pthread_mutex_unlock_rc;
    }
    return __real_pthread_mutex_unlock(mu);
}

extern int
__real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int
__wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
    if (g_mock.armed_pthread_cond_wait) {
        g_mock.armed_pthread_cond_wait = 0;
        return g_mock.pthread_cond_wait_rc;
    }
    return __real_pthread_cond_wait(cv, mu);
}

extern int
__real_pthread_cond_signal(pthread_cond_t *);
int
__wrap_pthread_cond_signal(pthread_cond_t *cv)
{
    if (g_mock.armed_pthread_cond_signal) {
        g_mock.armed_pthread_cond_signal = 0;
        return g_mock.pthread_cond_signal_rc;
    }
    return __real_pthread_cond_signal(cv);
}

extern int
__real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int
__wrap_pthread_mutex_init(pthread_mutex_t *mu, const pthread_mutexattr_t *attr)
{
    if (g_mock.armed_pthread_mutex_init) {
        g_mock.armed_pthread_mutex_init = 0;
        return g_mock.pthread_mutex_init_rc;
    }
    return __real_pthread_mutex_init(mu, attr);
}

extern int
__real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int
__wrap_pthread_cond_init(pthread_cond_t *cv, const pthread_condattr_t *attr)
{
    if (g_mock.armed_pthread_cond_init) {
        g_mock.armed_pthread_cond_init = 0;
        return g_mock.pthread_cond_init_rc;
    }
    return __real_pthread_cond_init(cv, attr);
}

extern int
__real_pthread_mutex_destroy(pthread_mutex_t *);
int
__wrap_pthread_mutex_destroy(pthread_mutex_t *mu)
{
    if (g_mock.armed_pthread_mutex_destroy) {
        g_mock.armed_pthread_mutex_destroy = 0;
        return g_mock.pthread_mutex_destroy_rc;
    }
    return __real_pthread_mutex_destroy(mu);
}

extern int
__real_pthread_cond_destroy(pthread_cond_t *);
int
__wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
    if (g_mock.armed_pthread_cond_destroy) {
        g_mock.armed_pthread_cond_destroy = 0;
        return g_mock.pthread_cond_destroy_rc;
    }
    return __real_pthread_cond_destroy(cv);
}

extern void
__real_abort(void);
void
__wrap_abort(void)
{
    if (g_abort_expect) {
        g_abort_expect = 0;
        longjmp(g_abort_jmp, 1);
    }
    if (g_mock.abort_exit_code > 0) {
        _exit(g_mock.abort_exit_code);
    }
    __real_abort();
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

static void
t_cache_idle_helpers(void **state)
{
    (void)state;

    nsgai_channel_t e1, e2, e3;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));
    memset(&e3, 0, sizeof(e3));

    e1.refcount = 0;
    e1.lru_seq  = 20;
    e1.next     = &e2;

    e2.refcount = 1; /* busy entry must be ignored */
    e2.lru_seq  = 10;
    e2.next     = &e3;

    e3.refcount = 0;
    e3.lru_seq  = 5;
    e3.next     = NULL;

    g_cache_head = &e1;
    g_cache_idle = 2;

    nsgai_channel_t *victim = cache_find_lru_idle();
    assert_ptr_equal(victim, &e3);

    g_cache_idle = NSGAI_CACHE_MAX_IDLE;
    assert_null(cache_maybe_evict_locked());

    g_cache_idle = NSGAI_CACHE_MAX_IDLE + 1;
    victim       = cache_maybe_evict_locked();
    assert_ptr_equal(victim, &e3);
    assert_int_equal(g_cache_idle, NSGAI_CACHE_MAX_IDLE);
    assert_ptr_equal(g_cache_head, &e1);
    assert_ptr_equal(e1.next, &e2);

    cache_unlink(&e1);
    assert_ptr_equal(g_cache_head, &e2);

    /* Cleanup globals so later tests start from known empty state. */
    g_cache_head = NULL;
    g_cache_idle = 0;
}

static void
t_entry_is_fresh_variants(void **state)
{
    (void)state;

    char templ[] = "/tmp/nsgai-fresh-XXXXXX";
    int fd       = mkstemp(templ);
    assert_int_not_equal(fd, -1);
    close(fd);

    nsgai_channel_t e;
    memset(&e, 0, sizeof(e));
    e.resolvconf_path = templ;
    e.resolv_valid    = true;
    assert_int_equal(stat(templ, &e.resolv_st), 0);
    assert_true(entry_is_fresh(&e));

    /* Change tracked metadata to force stat_changed() path. */
    struct timespec ts[2];
    ts[0].tv_sec  = e.resolv_st.st_atim.tv_sec;
    ts[0].tv_nsec = e.resolv_st.st_atim.tv_nsec;
    ts[1].tv_sec  = e.resolv_st.st_mtim.tv_sec + 1;
    ts[1].tv_nsec = e.resolv_st.st_mtim.tv_nsec;
    assert_int_equal(utimensat(AT_FDCWD, templ, ts, 0), 0);
    assert_false(entry_is_fresh(&e));

    unlink(templ);

    memset(&e, 0, sizeof(e));
    e.hosts_valid = true;
    e.hosts_path  = "/definitely/no/such/file";
    assert_false(entry_is_fresh(&e));
}

static void
t_channel_create_uncached_paths(void **state)
{
    (void)state;

    nsgai_ns_t ns;
    assert_int_equal(nsgai_ns_resolve_implicit(&ns), 0);

    nsgai_channel_t *ch = NULL;
    assert_int_equal(nsgai_channel_create_uncached(&ns, &ch), ARES_SUCCESS);
    assert_non_null(ch);
    nsgai_channel_destroy(ch);
    nsgai_ns_release(&ns);

    nsgai_ns_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.netns_fd = 987654; /* invalid fd for compute_ns_key() */
    ch           = NULL;
    assert_int_equal(nsgai_channel_create_uncached(&bad, &ch), ARES_EFILE);
    assert_null(ch);
}

static void
t_addrinfo_hosts_and_service_helpers(void **state)
{
    (void)state;

    char hosts_templ[] = "/tmp/nsgai-hosts-XXXXXX";
    int fd             = mkstemp(hosts_templ);
    assert_int_not_equal(fd, -1);

    const char *contents = "127.0.0.9 host.example.com host\n"
                           "::1 ip6.example.com ip6\n";
    ssize_t nw           = write(fd, contents, strlen(contents));
    assert_int_equal((size_t)nw, strlen(contents));
    close(fd);

    nsgai_ns_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.netns_fd   = -1;
    ns.hosts_path = hosts_templ;

    struct sockaddr_in sa4;
    memset(&sa4, 0, sizeof(sa4));
    sa4.sin_family = AF_INET;
    assert_int_equal(inet_pton(AF_INET, "127.0.0.9", &sa4.sin_addr), 1);

    char host[128];
    assert_int_equal(nsgai_lookup_hosts_reverse(&ns, (struct sockaddr *)&sa4,
                         sizeof(sa4), host, sizeof(host), 0),
        0);
    assert_string_equal(host, "host.example.com");

    assert_int_equal(nsgai_lookup_hosts_reverse(&ns, (struct sockaddr *)&sa4,
                         sizeof(sa4), host, sizeof(host), NI_NOFQDN),
        0);
    assert_string_equal(host, "host");

    char tiny[2];
    assert_int_equal(nsgai_lookup_hosts_reverse(&ns, (struct sockaddr *)&sa4,
                         sizeof(sa4), tiny, sizeof(tiny), 0),
        EAI_OVERFLOW);

    struct sockaddr_in miss;
    memset(&miss, 0, sizeof(miss));
    miss.sin_family = AF_INET;
    assert_int_equal(inet_pton(AF_INET, "127.0.0.10", &miss.sin_addr), 1);
    assert_int_equal(nsgai_lookup_hosts_reverse(&ns, (struct sockaddr *)&miss,
                         sizeof(miss), host, sizeof(host), 0),
        EAI_NONAME);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_CANONNAME;

    struct addrinfo *res = NULL;
    assert_int_equal(
        nsgai_lookup_hosts_forward(&ns, "host", "80", &hints, &res), 0);
    assert_non_null(res);
    assert_non_null(res->ai_canonname);
    nsgai_freeaddrinfo(res);

    res = NULL;
    assert_int_equal(
        nsgai_lookup_hosts_forward(&ns, "no-such-host", "80", &hints, &res),
        EAI_NONAME);
    assert_null(res);

    char serv[NI_MAXSERV];
    memset(serv, 0, sizeof(serv));
    sa4.sin_port = htons(53);
    assert_int_equal(
        nsgai_format_service((struct sockaddr *)&sa4, serv, sizeof(serv), 0),
        0);
    assert_true(serv[0] != '\0');

    assert_int_equal(nsgai_format_service((struct sockaddr *)&sa4, serv,
                         sizeof(serv), NI_NUMERICSERV),
        0);
    assert_string_equal(serv, "53");

    char serv_tiny[2];
    assert_int_equal(nsgai_format_service((struct sockaddr *)&sa4, serv_tiny,
                         sizeof(serv_tiny), NI_NUMERICSERV),
        EAI_OVERFLOW);

    struct sockaddr_storage bad;
    memset(&bad, 0, sizeof(bad));
    bad.ss_family = AF_UNSPEC;
    assert_int_equal(
        nsgai_format_service((struct sockaddr *)&bad, serv, sizeof(serv), 0),
        EAI_FAMILY);

    assert_int_equal(
        nsgai_format_service((struct sockaddr *)&sa4, NULL, 0, NI_NUMERICSERV),
        0);

    unlink(hosts_templ);
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
    assert_int_equal(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY), 0);
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
    assert_int_equal(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST), 0);
}

static void
t_do_getnameinfo_channel_fail(void **state)
{
    (void)state;
    assert_int_equal(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY), 0);
    g_mock.armed_ares_init  = 1;
    g_mock.ares_init_status = ARES_ENOMEM;
    struct sockaddr_in sa   = {0};
    sa.sin_family           = AF_INET;
    sa.sin_addr.s_addr      = htonl(0x7f000001);
    char host[64];
    int rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
    assert_int_equal(rc, EAI_MEMORY);
    assert_int_equal(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST), 0);
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
    assert_int_equal(
        nsgai_getaddrinfo_ns(NULL, "localhost", NULL, NULL, &res), 0);
    nsgai_freeaddrinfo(res);
    res = NULL;
    /* Bad ns name -> EAI_SYSTEM. */
    assert_int_equal(
        nsgai_getaddrinfo_ns("a/b", "localhost", NULL, NULL, &res), EAI_SYSTEM);
    /* Negative fd falls back to implicit resolution. */
    assert_int_equal(
        nsgai_getaddrinfo_nsfd(-1, "localhost", NULL, NULL, &res), 0);
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

static void
t_addrinfo_helper_edges(void **state)
{
    (void)state;

    char comment_line[] = "127.0.0.1 host # trailing comment";
    nsgai_hosts_strip_comment(comment_line);
    assert_string_equal(comment_line, "127.0.0.1 host ");

    char tokens[] = "   alpha beta";
    char *cursor  = tokens;
    char *tok     = NULL;
    assert_int_equal(nsgai_getline_token(&cursor, &tok), 1);
    assert_string_equal(tok, "alpha");
    assert_int_equal(nsgai_getline_token(&cursor, &tok), 1);
    assert_string_equal(tok, "beta");
    assert_int_equal(nsgai_getline_token(&cursor, &tok), 0);
    assert_null(tok);

    struct addrinfo *res = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    assert_int_equal(
        nsgai_synthesize_null_node_addrinfo(NULL, NULL, &res), EAI_NONAME);

    hints.ai_family = AF_UNIX;
    assert_int_equal(
        nsgai_synthesize_null_node_addrinfo("53", &hints, &res), EAI_FAMILY);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags  = AI_PASSIVE;
    assert_int_equal(
        nsgai_synthesize_null_node_addrinfo("53", &hints, &res), 0);
    assert_non_null(res);
    assert_non_null(res->ai_next);
    nsgai_freeaddrinfo(res);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    res             = NULL;
    assert_int_equal(
        nsgai_numeric_node_to_addrinfo("::1", "0", &hints, &res), 0);
    assert_non_null(res);
    assert_int_equal(res->ai_family, AF_INET6);
    nsgai_freeaddrinfo(res);

    hints.ai_family = AF_INET;
    res             = NULL;
    assert_int_equal(
        nsgai_numeric_node_to_addrinfo("::1", "0", &hints, &res), EAI_NONAME);
    assert_null(res);

    struct sockaddr_in6 sa6;
    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    assert_int_equal(inet_pton(AF_INET6, "::1", &sa6.sin6_addr), 1);
    char host[NI_MAXHOST];
    assert_int_equal(
        nsgai_format_numeric_host((struct sockaddr *)&sa6, host, sizeof(host)),
        0);
    assert_true(strchr(host, ':') != NULL);

    struct sockaddr_storage bad;
    memset(&bad, 0, sizeof(bad));
    bad.ss_family = AF_UNSPEC;
    assert_int_equal(
        nsgai_format_numeric_host((struct sockaddr *)&bad, host, sizeof(host)),
        EAI_FAMILY);

    nsgai_ns_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.netns_fd   = -1;
    ns.hosts_path = "/definitely/no/such/hosts";
    assert_int_equal(
        nsgai_lookup_hosts_forward(&ns, "x", "80", NULL, &res), EAI_NONAME);

    char hosts_templ[] = "/tmp/nsgai-helpers-XXXXXX";
    int fd             = mkstemp(hosts_templ);
    assert_int_not_equal(fd, -1);
    const char *contents = "# comment-only line\n"
                           "::1 v6host.example v6host # inline comment\n"
                           "127.0.0.9 v4host.example v4host\n";
    assert_int_equal(
        (size_t)write(fd, contents, strlen(contents)), strlen(contents));
    close(fd);

    ns.hosts_path = hosts_templ;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    assert_int_equal(
        nsgai_lookup_hosts_forward(&ns, "v6host", "80", &hints, &res), 0);
    assert_non_null(res);
    assert_int_equal(res->ai_family, AF_INET6);
    nsgai_freeaddrinfo(res);

    assert_int_equal(
        nsgai_lookup_hosts_forward(&ns, "v6host", "999999", &hints, &res),
        EAI_SERVICE);

    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    assert_int_equal(inet_pton(AF_INET6, "::1", &sa6.sin6_addr), 1);
    assert_int_equal(nsgai_lookup_hosts_reverse(&ns, (struct sockaddr *)&sa6,
                         sizeof(sa6), host, sizeof(host), NI_NOFQDN),
        0);
    assert_string_equal(host, "v6host");

    g_mock.calloc_fail_nth   = 1;
    g_mock.calloc_call_count = 0;
    res = (struct addrinfo *)new_ai_node(AF_INET, SOCK_STREAM, IPPROTO_TCP,
        &((struct in_addr){.s_addr = htonl(INADDR_LOOPBACK)}),
        sizeof(struct in_addr), 0, NULL);
    if (g_mock.calloc_call_count != 0) {
        assert_null(res);
    } else {
        nsgai_freeaddrinfo(res);
    }

    g_mock.calloc_fail_nth   = 2;
    g_mock.calloc_call_count = 0;
    res = (struct addrinfo *)new_ai_node(AF_INET, SOCK_STREAM, IPPROTO_TCP,
        &((struct in_addr){.s_addr = htonl(INADDR_LOOPBACK)}),
        sizeof(struct in_addr), 0, NULL);
    if (g_mock.calloc_call_count != 0) {
        assert_null(res);
    } else {
        nsgai_freeaddrinfo(res);
    }
    g_mock.calloc_fail_nth = 0;

    unlink(hosts_templ);
}

static void
t_addrinfo_policy_edges(void **state)
{
    (void)state;

    char hosts_templ[] = "/tmp/nsgai-policy-XXXXXX";
    int fd             = mkstemp(hosts_templ);
    assert_int_not_equal(fd, -1);
    const char *contents = "127.0.0.9 known.example known\n";
    assert_int_equal(
        (size_t)write(fd, contents, strlen(contents)), strlen(contents));
    close(fd);

    nsgai_ns_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.netns_fd   = -1;
    ns.hosts_path = hosts_templ;

    struct addrinfo *res = NULL;
    assert_int_equal(
        do_getaddrinfo(&ns, "known", "80", NULL, NSGAI_LOOKUP_HOSTS_ONLY, &res),
        0);
    assert_non_null(res);
    nsgai_freeaddrinfo(res);

    res = NULL;
    assert_int_equal(do_getaddrinfo(&ns, "missing-host", "80", NULL,
                         NSGAI_LOOKUP_HOSTS_ONLY, &res),
        EAI_NONAME);
    assert_null(res);

    struct sockaddr_in miss;
    memset(&miss, 0, sizeof(miss));
    miss.sin_family = AF_INET;
    miss.sin_port   = htons(514);
    assert_int_equal(inet_pton(AF_INET, "127.0.0.10", &miss.sin_addr), 1);

    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    assert_int_equal(do_getnameinfo(&ns, (struct sockaddr *)&miss, sizeof(miss),
                         host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST,
                         NSGAI_LOOKUP_HOSTS_ONLY),
        0);
    assert_string_equal(host, "127.0.0.10");

    assert_int_equal(
        do_getnameinfo(&ns, (struct sockaddr *)&miss, sizeof(miss), host,
            sizeof(host), NULL, 0, 0, NSGAI_LOOKUP_HOSTS_ONLY),
        EAI_NONAME);

    assert_int_equal(
        do_getnameinfo(&ns, (struct sockaddr *)&miss, sizeof(miss), host,
            sizeof(host), NULL, 0, NI_NAMEREQD, NSGAI_LOOKUP_HOSTS_ONLY),
        EAI_NONAME);

    unlink(hosts_templ);
}

static void
t_channel_build_paths_and_socket_fn_failure(void **state)
{
    (void)state;

    char resolv_templ[] = "/tmp/nsgai-resolv-XXXXXX";
    int rfd             = mkstemp(resolv_templ);
    assert_int_not_equal(rfd, -1);
    const char *resolv_contents = "nameserver 127.0.0.1\n";
    assert_int_equal(
        (size_t)write(rfd, resolv_contents, strlen(resolv_contents)),
        strlen(resolv_contents));
    close(rfd);

    char hosts_templ[] = "/tmp/nsgai-hosts2-XXXXXX";
    int hfd            = mkstemp(hosts_templ);
    assert_int_not_equal(hfd, -1);
    const char *hosts_contents = "127.0.0.1 localhost\n";
    assert_int_equal((size_t)write(hfd, hosts_contents, strlen(hosts_contents)),
        strlen(hosts_contents));
    close(hfd);

    nsgai_ns_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.netns_fd        = -1;
    ns.resolvconf_path = resolv_templ;
    ns.hosts_path      = hosts_templ;

    nsgai_channel_t *ch = NULL;
    assert_int_equal(nsgai_channel_create_uncached(&ns, &ch), ARES_SUCCESS);
    assert_non_null(ch);
    assert_true(ch->resolv_valid);
    assert_true(ch->hosts_valid);
    nsgai_channel_destroy(ch);

#ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX
    g_mock.armed_ares_set_socket_functions_ex  = 1;
    g_mock.ares_set_socket_functions_ex_status = ARES_EFORMERR;
    ch                                         = NULL;
    assert_int_equal(nsgai_channel_create_uncached(&ns, &ch), ARES_EFORMERR);
    assert_null(ch);
#endif   // ifdef NSGAI_HAVE_CARES_SOCKFUNCS_EX

    unlink(resolv_templ);
    unlink(hosts_templ);
}

static void
t_channel_cache_mismatch_and_stale_paths(void **state)
{
    (void)state;

    nsgai_ns_t implicit;
    assert_int_equal(nsgai_ns_resolve_implicit(&implicit), 0);

    nsgai_channel_t *ch = NULL;
    assert_int_equal(nsgai_channel_create(&implicit, &ch), ARES_SUCCESS);
    assert_non_null(ch);
    nsgai_channel_destroy(ch);

    /* Stale entry path in scan loop. */
    if (g_cache_head != NULL) {
        g_cache_head->stale = true;
    }
    assert_int_equal(nsgai_channel_create(&implicit, &ch), ARES_SUCCESS);
    assert_non_null(ch);
    nsgai_channel_destroy(ch);

    int netfd = open_self_netns();
    assert_int_not_equal(netfd, -1);
    nsgai_ns_t explicit_fd;
    assert_int_equal(nsgai_ns_resolve_explicit_fd(netfd, &explicit_fd), 0);
    assert_int_equal(nsgai_channel_create(&explicit_fd, &ch), ARES_SUCCESS);
    assert_non_null(ch);
    nsgai_channel_destroy(ch);
    nsgai_ns_release(&explicit_fd);
    close(netfd);

    char hosts_templ[] = "/tmp/nsgai-hosts-mismatch-XXXXXX";
    int hfd            = mkstemp(hosts_templ);
    assert_int_not_equal(hfd, -1);
    const char *hosts_contents = "127.0.0.1 localhost\n";
    assert_int_equal((size_t)write(hfd, hosts_contents, strlen(hosts_contents)),
        strlen(hosts_contents));
    close(hfd);

    nsgai_ns_t with_hosts = implicit;
    with_hosts.hosts_path = hosts_templ;
    assert_int_equal(nsgai_channel_create(&with_hosts, &ch), ARES_SUCCESS);
    assert_non_null(ch);
    nsgai_channel_destroy(ch);

    unlink(hosts_templ);
    nsgai_ns_release(&implicit);

    nsgai_ns_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.netns_fd = 987654;
    assert_int_equal(nsgai_channel_create(&bad, &ch), ARES_EFILE);
}

static int
expect_abort_jump(void (*fn)(void))
{
    if (setjmp(g_abort_jmp) == 0) {
        g_abort_expect = 1;
        fn();
        g_abort_expect = 0;
        return -1;
    }
    return 0;
}

static void
child_fail_lock_abort(void)
{
    pthread_mutex_t mu              = PTHREAD_MUTEX_INITIALIZER;
    g_mock.armed_pthread_mutex_lock = 1;
    g_mock.pthread_mutex_lock_rc    = EPERM;
    nsgai_pthread_mutex_lock_or_abort(&mu, "lock");
}

static void
child_fail_unlock_abort(void)
{
    pthread_mutex_t mu                = PTHREAD_MUTEX_INITIALIZER;
    g_mock.armed_pthread_mutex_unlock = 1;
    g_mock.pthread_mutex_unlock_rc    = EPERM;
    nsgai_pthread_mutex_unlock_or_abort(&mu, "unlock");
}

static void
child_fail_wait_abort(void)
{
    pthread_mutex_t mu             = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv              = PTHREAD_COND_INITIALIZER;
    g_mock.armed_pthread_cond_wait = 1;
    g_mock.pthread_cond_wait_rc    = EPERM;
    nsgai_pthread_cond_wait_or_abort(&cv, &mu, "wait");
}

static void
child_fail_signal_abort(void)
{
    pthread_cond_t cv                = PTHREAD_COND_INITIALIZER;
    g_mock.armed_pthread_cond_signal = 1;
    g_mock.pthread_cond_signal_rc    = EPERM;
    nsgai_pthread_cond_signal_or_abort(&cv, "signal");
}

static void
t_diag_abort_and_log_paths(void **state)
{
    (void)state;

    assert_int_equal(expect_abort_jump(child_fail_lock_abort), 0);
    assert_int_equal(expect_abort_jump(child_fail_unlock_abort), 0);
    assert_int_equal(expect_abort_jump(child_fail_wait_abort), 0);
    assert_int_equal(expect_abort_jump(child_fail_signal_abort), 0);

    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv  = PTHREAD_COND_INITIALIZER;

    g_mock.armed_pthread_mutex_init = 1;
    g_mock.pthread_mutex_init_rc    = EAGAIN;
    assert_int_equal(nsgai_pthread_mutex_init_checked(&mu, "mi"), EAGAIN);

    g_mock.armed_pthread_cond_init = 1;
    g_mock.pthread_cond_init_rc    = EAGAIN;
    assert_int_equal(nsgai_pthread_cond_init_checked(&cv, "ci"), EAGAIN);

    g_mock.armed_pthread_mutex_destroy = 1;
    g_mock.pthread_mutex_destroy_rc    = EBUSY;
    nsgai_pthread_mutex_destroy_checked(&mu, "md");

    g_mock.armed_pthread_cond_destroy = 1;
    g_mock.pthread_cond_destroy_rc    = EBUSY;
    nsgai_pthread_cond_destroy_checked(&cv, "cd");
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
        cmocka_unit_test_setup(t_cache_idle_helpers, setup),
        cmocka_unit_test_setup(t_entry_is_fresh_variants, setup),
        cmocka_unit_test_setup(t_channel_create_uncached_paths, setup),
        cmocka_unit_test_setup(t_addrinfo_hosts_and_service_helpers, setup),
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
        cmocka_unit_test_setup(t_addrinfo_helper_edges, setup),
        cmocka_unit_test_setup(t_addrinfo_policy_edges, setup),
        cmocka_unit_test_setup(
            t_channel_build_paths_and_socket_fn_failure, setup),
        cmocka_unit_test_setup(t_channel_cache_mismatch_and_stale_paths, setup),
        cmocka_unit_test_setup(t_diag_abort_and_log_paths, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
