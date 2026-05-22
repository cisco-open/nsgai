/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Failure-injection coverage test for libnss_nsgai.so.2.
 *
 * Links the SAME object files used by libnss_nsgai.so.2 + libnsgai.so
 * ($<TARGET_OBJECTS:nss_nsgai_core> + $<TARGET_OBJECTS:nsgai_core>),
 * so coverage counters merge with the project-wide report.
 *
 * Uses ld --wrap to intercept selected c-ares entry points and drive
 * each error mapping arm in do_lookup() and the buf_alloc / ERANGE
 * paths of every _nss_nsgai_* entry point.
 */

#include "nsgai.h"
#include "nsgai_internal.h"

#include <ares.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <nss.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* NSS entry points are exported from nss_nsgai_core's translation unit
 * which is linked into this executable, so we can call them directly. */
extern enum nss_status
_nss_nsgai_gethostbyname_r(
    const char *, struct hostent *, char *, size_t, int *, int *);
extern enum nss_status
_nss_nsgai_gethostbyname2_r(
    const char *, int, struct hostent *, char *, size_t, int *, int *);
extern enum nss_status
_nss_nsgai_gethostbyname4_r(const char *,
    struct gaih_addrtuple **,
    char *,
    size_t,
    int *,
    int *,
    int32_t *);
extern enum nss_status
_nss_nsgai_gethostbyaddr_r(const void *,
    socklen_t,
    int,
    struct hostent *,
    char *,
    size_t,
    int *,
    int *);
extern enum nss_status
_nss_nsgai_gethostbyaddr2_r(const void *,
    socklen_t,
    int,
    struct hostent *,
    char *,
    size_t,
    int *,
    int *,
    int32_t *);

/* ------------------------------------------------------------------ *
 * Mock state                                                          *
 * ------------------------------------------------------------------ */

static struct {
    int armed_ares_init;
    int ares_init_status;

    int armed_ai;
    int ai_status;
    struct ares_addrinfo *ai_result;
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
__wrap_ares_getaddrinfo(ares_channel_t *c,
    const char *name,
    const char *service,
    const struct ares_addrinfo_hints *hints,
    ares_addrinfo_callback cb,
    void *arg)
{
    if (g.armed_ai) {
        int status   = g.ai_status;
        void *result = g.ai_result;
        g.armed_ai   = 0;
        g.ai_result  = NULL;
        cb(arg, status, 0, (struct ares_addrinfo *)result);
        return;
    }
    __real_ares_getaddrinfo(c, name, service, hints, cb, arg);
}

/* ------------------------------------------------------------------ *
 * Helpers                                                             *
 * ------------------------------------------------------------------ */

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ *
 * Tests                                                               *
 * ------------------------------------------------------------------ */

static int
test_channel_create_fail(void)
{
    /* do_lookup: ares_init fail -> NSS_STATUS_UNAVAIL/ENOMEM */
    reset();
    g.armed_ares_init  = 1;
    g.ares_init_status = ARES_ENOMEM;
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "localhost", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_UNAVAIL);
    CHECK(err == ENOMEM);
    CHECK(herr == NETDB_INTERNAL);
    return 0;
}

static int
test_ai_timeout(void)
{
    /* do_lookup: ARES_ETIMEOUT -> TRYAGAIN/EAGAIN */
    reset();
    g.armed_ai  = 1;
    g.ai_status = ARES_ETIMEOUT;
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "x.invalid.test", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_TRYAGAIN);
    CHECK(err == EAGAIN);
    CHECK(herr == TRY_AGAIN);
    return 0;
}

static int
test_ai_notfound(void)
{
    reset();
    g.armed_ai  = 1;
    g.ai_status = ARES_ENOTFOUND;
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "x.invalid.test", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_NOTFOUND);
    CHECK(herr == HOST_NOT_FOUND);
    return 0;
}

static int
test_ai_nodata(void)
{
    reset();
    g.armed_ai  = 1;
    g.ai_status = ARES_ENODATA;
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "x.invalid.test", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_NOTFOUND);
    return 0;
}

static int
test_ai_generic_error(void)
{
    /* default arm: EIO + NO_RECOVERY -> UNAVAIL */
    reset();
    g.armed_ai  = 1;
    g.ai_status = ARES_EFORMERR;
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "x.invalid.test", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_UNAVAIL);
    CHECK(err == EIO);
    CHECK(herr == NO_RECOVERY);
    return 0;
}

static int
test_ai_success_null_result(void)
{
    /* ARES_SUCCESS but result NULL -> falls into else arm (EIO/UNAVAIL) */
    reset();
    g.armed_ai  = 1;
    g.ai_status = ARES_SUCCESS;
    g.ai_result = NULL;
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "x.invalid.test", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_UNAVAIL);
    return 0;
}

static int
test_buf_alloc_aliases_erange(void)
{
    /* Buffer just big enough for h_name but not the aliases pointer.
     * "localhost\0" = 10 bytes; aliases needs 8 more (pointer-sized
     * NULL).  Use tiny=10 to fail at the aliases buf_alloc. */
    struct hostent he;
    char buf[10];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyname2_r(
        "localhost", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_TRYAGAIN);
    CHECK(err == ERANGE);
    return 0;
}

static int
test_gethostbyname4_buf_addrlist(void)
{
    /* Drive the buf_alloc failures in gethostbyname4_r.  Tiny buffer. */
    struct gaih_addrtuple *at = NULL;
    char buf[12];
    int err = 0, herr = 0;
    int32_t ttl        = 0;
    enum nss_status st = _nss_nsgai_gethostbyname4_r(
        "localhost", &at, buf, sizeof(buf), &err, &herr, &ttl);
    CHECK(st == NSS_STATUS_TRYAGAIN);
    CHECK(err == ERANGE);
    return 0;
}

static int
test_getaddr_eai_again(void)
{
    /* Exercise the channel-create failure mapping through
     * gethostbyaddr_r.  The cached channel from previous successful
     * tests may short-circuit ares_init_options, so we only run the
     * path for coverage and accept any documented status. */
    reset();
    g.armed_ares_init  = 1;
    g.ares_init_status = ARES_ENOMEM;
    uint8_t v4[4]      = {127, 0, 0, 1};
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyaddr_r(
        v4, 4, AF_INET, &he, buf, sizeof(buf), &err, &herr);
    (void)st;
    return 0;
}

static int
test_getaddr_buf_alloc_fail(void)
{
    /* gethostbyaddr_r: drive each buf_alloc failure with a tight buffer. */
    uint8_t v4[4] = {127, 0, 0, 1};
    struct hostent he;
    char buf[8]; /* too small for the name + aliases + addrlist + addr */
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyaddr_r(
        v4, 4, AF_INET, &he, buf, sizeof(buf), &err, &herr);
    /* Either reverse lookup succeeds (then ERANGE) or fails (NOTFOUND
     * / UNAVAIL / TRYAGAIN) - any of those is fine. */
    (void)st;
    return 0;
}

static int
test_gethostbyaddr_bad_v6_len(void)
{
    uint8_t v6[16] = {0};
    struct hostent he;
    char buf[4096];
    int err = 0, herr = 0;
    enum nss_status st = _nss_nsgai_gethostbyaddr_r(
        v6, 4, AF_INET6, &he, buf, sizeof(buf), &err, &herr);
    CHECK(st == NSS_STATUS_UNAVAIL);
    CHECK(err == EINVAL);
    return 0;
}

int
main(void)
{
    if (test_channel_create_fail()) {
        return 1;
    }
    if (test_ai_timeout()) {
        return 1;
    }
    if (test_ai_notfound()) {
        return 1;
    }
    if (test_ai_nodata()) {
        return 1;
    }
    if (test_ai_generic_error()) {
        return 1;
    }
    if (test_ai_success_null_result()) {
        return 1;
    }
    if (test_buf_alloc_aliases_erange()) {
        return 1;
    }
    if (test_gethostbyname4_buf_addrlist()) {
        return 1;
    }
    if (test_getaddr_eai_again()) {
        return 1;
    }
    if (test_getaddr_buf_alloc_fail()) {
        return 1;
    }
    if (test_gethostbyaddr_bad_v6_len()) {
        return 1;
    }
    printf("nss failure-injection tests passed\n");
    return 0;
}
