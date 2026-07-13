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
 * Failure-injection coverage test.
 *
 * Unlike test_channel_mocks (which #includes the SUT and has its own
 * private compilation), this binary links the SAME object files that
 * libnsgai.so is built from ($<TARGET_OBJECTS:nsgai_core>).  Coverage
 * counters therefore land in the same .gcda files as the rest of the
 * suite, so the error branches we hit here are reflected in the
 * project-wide coverage report.
 *
 * We use ld's --wrap mechanism (configured in CMakeLists.txt) to
 * intercept selected libc / c-ares symbols so we can fail them on
 * demand.  Every wrapper defaults to a pass-through via __real_NAME()
 * unless armed.
 */

#include "nsgai.h"
#include "nsgai_internal.h"

#include <arpa/inet.h>
#include <ares.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Arming                                                              *
 * ------------------------------------------------------------------ */

static struct {
    /* ares_init_options(): one-shot fail with this status. */
    int armed_ares_init;
    int ares_init_status;

    /* fcntl(F_DUPFD_CLOEXEC): fail the N-th such call (1-based). */
    int armed_fcntl_dup_nth;
    int fcntl_dup_count;

    /* ares_getaddrinfo(): optionally complete immediately without DNS I/O. */
    int armed_ares_getaddrinfo_immediate;
    int saw_ares_getaddrinfo;

    /* ares_destroy(): count channel destruction. */
    int ares_destroy_count;
} g;

static void
reset(void)
{
    memset(&g, 0, sizeof(g));
}

/* ------------------------------------------------------------------ *
 * Wrappers                                                            *
 * ------------------------------------------------------------------ */

extern ares_status_t
__real_ares_init_options(ares_channel_t **, const struct ares_options *, int);
ares_status_t
__wrap_ares_init_options(
    ares_channel_t **c, const struct ares_options *o, int m)
{
    if (g.armed_ares_init) {
        ares_status_t st  = (ares_status_t)g.ares_init_status;
        g.armed_ares_init = 0;
        return st;
    }
    return __real_ares_init_options(c, o, m);
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
    ares_addrinfo_callback cb,
    void *arg)
{
    g.saw_ares_getaddrinfo = 1;
    if (g.armed_ares_getaddrinfo_immediate) {
        g.armed_ares_getaddrinfo_immediate = 0;
        cb(arg, ARES_ENOTFOUND, 0, NULL);
        return;
    }
    __real_ares_getaddrinfo(channel, name, service, hints, cb, arg);
}

extern void
__real_ares_destroy(ares_channel_t *);
void
__wrap_ares_destroy(ares_channel_t *channel)
{
    g.ares_destroy_count++;
    __real_ares_destroy(channel);
}

extern int
__real_fcntl(int, int, ...);
int
__wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (cmd == F_DUPFD_CLOEXEC) {
        g.fcntl_dup_count++;
        if (g.armed_fcntl_dup_nth > 0 &&
            g.fcntl_dup_count == g.armed_fcntl_dup_nth) {
            errno = EMFILE;
            return -1;
        }
    }
    return __real_fcntl(fd, cmd, arg);
}

/* ------------------------------------------------------------------ *
 * Tests                                                               *
 * ------------------------------------------------------------------ */

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int
test_ares_init_enomem(void)
{
    reset();
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY) == 0);
    g.armed_ares_init  = 1;
    g.ares_init_status = ARES_ENOMEM;
    struct addrinfo *r = NULL;
    int rc             = nsgai_getaddrinfo("localhost", NULL, NULL, &r);
    CHECK(rc == EAI_MEMORY);
    CHECK(r == NULL);
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST) == 0);
    return 0;
}

static int
test_ares_init_generic(void)
{
    reset();
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY) == 0);
    g.armed_ares_init  = 1;
    g.ares_init_status = ARES_ENOTINITIALIZED;
    struct addrinfo *r = NULL;
    int rc             = nsgai_getaddrinfo("localhost", NULL, NULL, &r);
    CHECK(rc == EAI_SYSTEM);
    CHECK(r == NULL);
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST) == 0);
    return 0;
}

static int
test_getnameinfo_channel_fail(void)
{
    reset();
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY) == 0);
    g.armed_ares_init     = 1;
    g.ares_init_status    = ARES_ENOMEM;
    struct sockaddr_in sa = {0};
    sa.sin_family         = AF_INET;
    sa.sin_addr.s_addr    = htonl(0x7f000001);
    char host[64];
    int rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
    CHECK(rc == EAI_MEMORY);
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST) == 0);
    return 0;
}

static int
test_fcntl_dup_fail_in_resolve_fd(void)
{
    /* nsgai_ns_resolve_explicit_fd calls fcntl(ns_fd, F_DUPFD_CLOEXEC, 3)
     * before anything else.  Fail the 1st F_DUPFD_CLOEXEC and the public
     * API must surface EAI_SYSTEM. */
    int nsfd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    CHECK(nsfd >= 0);
    reset();
    g.armed_fcntl_dup_nth = 1;
    struct addrinfo *r    = NULL;
    int rc = nsgai_getaddrinfo_nsfd(nsfd, "localhost", NULL, NULL, &r);
    CHECK(rc == EAI_SYSTEM);
    close(nsfd);
    return 0;
}

static int
test_fcntl_dup_fail_in_cache_build(void)
{
    /* For nsgai_getaddrinfo_nsfd, the call sequence performs two
     * F_DUPFD_CLOEXEC fcntls: one in nsgai_ns_resolve_explicit_fd, then
     * one inside cache_build_locked.  Failing the 2nd covers the
     * "free(e); return ARES_ENOMEM" branch of cache_build_locked. */
    int nsfd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    CHECK(nsfd >= 0);
    reset();
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY) == 0);
    g.armed_fcntl_dup_nth = 2;
    struct addrinfo *r    = NULL;
    int rc = nsgai_getaddrinfo_nsfd(nsfd, "localhost", NULL, NULL, &r);
    /* The path returns ARES_ENOMEM mapped to EAI_MEMORY. */
    CHECK(rc == EAI_MEMORY);
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST) == 0);
    close(nsfd);
    return 0;
}

static int
test_resolve_explicit_name_invalid(void)
{
    /* Drive validate_ns_name's negative branches via the public API.
     * Note: NULL ns_name now falls back to implicit resolution (valid).
     */
    struct addrinfo *r = NULL;
    /* Empty string, slashes, parent-dir, and non-existent name should fail. */
    CHECK(nsgai_getaddrinfo_ns("", "x", NULL, NULL, &r) == EAI_SYSTEM);
    CHECK(nsgai_getaddrinfo_ns("a/b", "x", NULL, NULL, &r) == EAI_SYSTEM);
    CHECK(nsgai_getaddrinfo_ns("..", "x", NULL, NULL, &r) == EAI_SYSTEM);
    CHECK(nsgai_getaddrinfo_ns("does_not_exist_xyz_42", "x", NULL, NULL, &r) ==
          EAI_SYSTEM);
    return 0;
}

static int
test_resolve_explicit_fd_neg(void)
{
    /* Negative fd now falls back to implicit resolution (valid),
     * so we test that it succeeds rather than failing. */
    struct addrinfo *r = NULL;
    int rc = nsgai_getaddrinfo_nsfd(-1, "localhost", NULL, NULL, &r);
    CHECK(rc == 0);
    nsgai_freeaddrinfo(r);
    return 0;
}

static int
test_getnameinfo_overflow_serv(void)
{
    /* serv buffer too small -> EAI_OVERFLOW (the "second copy" arm
     * inside do_getnameinfo). */
    struct sockaddr_in sa = {0};
    sa.sin_family         = AF_INET;
    sa.sin_addr.s_addr    = htonl(0x7f000001);
    sa.sin_port           = htons(80);
    char host[64];
    char serv[2]; /* "80\0" needs 3 */
    int rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
    CHECK(rc == EAI_OVERFLOW);
    return 0;
}

static int
test_cleanup_destroys_idle_cached_channel(void)
{
    nsgai_cleanup();
    reset();
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_DNS_ONLY) == 0);
    g.armed_ares_getaddrinfo_immediate = 1;

    struct addrinfo *r = NULL;
    int rc             = nsgai_getaddrinfo("teardown.invalid", NULL, NULL, &r);
    CHECK(rc != 0);
    CHECK(r == NULL);
    CHECK(g.saw_ares_getaddrinfo);
    CHECK(g.ares_destroy_count == 0);

    nsgai_cleanup();
    CHECK(g.ares_destroy_count > 0);
    CHECK(nsgai_set_lookup_policy(NSGAI_LOOKUP_HOSTS_FIRST) == 0);
    return 0;
}

int
main(void)
{
    if (test_ares_init_enomem()) {
        return 1;
    }
    if (test_ares_init_generic()) {
        return 1;
    }
    if (test_getnameinfo_channel_fail()) {
        return 1;
    }
    if (test_fcntl_dup_fail_in_resolve_fd()) {
        return 1;
    }
    if (test_fcntl_dup_fail_in_cache_build()) {
        return 1;
    }
    if (test_resolve_explicit_name_invalid()) {
        return 1;
    }
    if (test_resolve_explicit_fd_neg()) {
        return 1;
    }
    if (test_getnameinfo_overflow_serv()) {
        return 1;
    }
    if (test_cleanup_destroys_idle_cached_channel()) {
        return 1;
    }
    printf("channel failure-injection tests passed\n");
    return 0;
}
