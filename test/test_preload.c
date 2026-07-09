/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Coverage-focused unit test for libnsgai_preload.so.
 *
 * This binary deliberately links ONLY against libc and calls the
 * POSIX-named entry points (getaddrinfo, getnameinfo, gai_strerror,
 * freeaddrinfo).  It is expected to be invoked with
 *   LD_PRELOAD=/path/to/libnsgai_preload.so
 * so that the preload shim wins symbol resolution at startup.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
/* clang-format off */
#include <cmocka.h>
/* clang-format on */

static void
t_preload_getaddrinfo(void **state)
{
    (void)state;

    /* getaddrinfo() should be intercepted by preload shim. */
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;
    struct addrinfo *res  = NULL;

    int rc = getaddrinfo("localhost", NULL, &hints, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res);
    freeaddrinfo(res);
}

static void
t_preload_getnameinfo(void **state)
{
    (void)state;

    /* getnameinfo() shim. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    char host[64];

    int rc = getnameinfo((struct sockaddr *)&sa, sizeof(sa), host, sizeof(host),
        NULL, 0, NI_NUMERICHOST);
    assert_int_equal(rc, 0);
    assert_string_equal(host, "127.0.0.1");
}

static void
t_preload_gai_strerror(void **state)
{
    (void)state;

    /* gai_strerror() shim - must return a non-empty string. */
    const char *s = gai_strerror(EAI_NONAME);
    assert_non_null(s);
    assert_true(*s != '\0');
}

static void
t_preload_gnu_async_aliases(void **state)
{
    (void)state;

    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;
    struct timespec timeout;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    /* Synchronous mode still flows through preload getaddrinfo_a alias. */
    assert_int_equal(getaddrinfo_a(GAI_WAIT, list, 1, NULL), 0);
    assert_int_equal(gai_error(&req), 0);
    assert_non_null(req.ar_result);
    freeaddrinfo(req.ar_result);

    timeout.tv_sec  = 0;
    timeout.tv_nsec = 1000000;
    assert_int_equal(
        gai_suspend((const struct gaicb *const *)list, 0, &timeout),
        EAI_ALLDONE);
    assert_int_equal(gai_cancel(&req), EAI_ALLDONE);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(t_preload_getaddrinfo),
        cmocka_unit_test(t_preload_getnameinfo),
        cmocka_unit_test(t_preload_gai_strerror),
        cmocka_unit_test(t_preload_gnu_async_aliases),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
