/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Coverage-focused unit test for the nsgai public API.  Exercises
 * branches in nsgai_gai_strerror, nsgai_map_ai_flags, nsgai_map_ni_flags,
 * resolve_service, nsgai_convert_addrinfo, and the explicit-ns input
 * validation paths.  No netns required.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define MUST(expr)                                                             \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int
test_gai_strerror(void)
{
    /* Hit every switch arm. */
    static const int codes[] = {
        0,
        EAI_AGAIN,
        EAI_BADFLAGS,
        EAI_FAIL,
        EAI_FAMILY,
        EAI_MEMORY,
        EAI_NONAME,
        EAI_SERVICE,
        EAI_SOCKTYPE,
        EAI_SYSTEM,
        EAI_OVERFLOW,
#ifdef EAI_ADDRFAMILY
        EAI_ADDRFAMILY,
#endif   // ifdef EAI_ADDRFAMILY
#ifdef EAI_NODATA
        EAI_NODATA,
#endif         // ifdef EAI_NODATA
        99999, /* default arm */
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *s = nsgai_gai_strerror(codes[i]);
        MUST(s && *s);
    }
    return 0;
}

static int
test_numeric_host(void)
{
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_flags        = AI_NUMERICHOST;
    struct addrinfo *res  = NULL;
    int rc                = nsgai_getaddrinfo("127.0.0.1", NULL, &hints, &res);
    MUST(rc == 0);
    MUST(res != NULL);
    MUST(res->ai_family == AF_INET);
    nsgai_freeaddrinfo(res);

    /* AI_NUMERICHOST with a non-numeric name must fail. */
    res = NULL;
    rc =
        nsgai_getaddrinfo("definitely-not-numeric.invalid", NULL, &hints, &res);
    MUST(rc != 0);
    if (res) {
        nsgai_freeaddrinfo(res);
    }
    return 0;
}

static int
test_numeric_serv(void)
{
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;
    hints.ai_flags        = AI_NUMERICSERV | AI_NUMERICHOST;
    struct addrinfo *res  = NULL;
    int rc = nsgai_getaddrinfo("127.0.0.1", "8080", &hints, &res);
    MUST(rc == 0);
    MUST(res != NULL);
    MUST(((struct sockaddr_in *)res->ai_addr)->sin_port == htons(8080));
    nsgai_freeaddrinfo(res);

    /* AI_NUMERICSERV with a non-numeric service must fail. */
    res = NULL;
    rc  = nsgai_getaddrinfo("127.0.0.1", "http", &hints, &res);
    MUST(rc != 0);
    if (res) {
        nsgai_freeaddrinfo(res);
    }
    return 0;
}

static int
test_service_resolution(void)
{
    /* Hit the SOCK_STREAM/"tcp" path in resolve_service via /etc/services. */
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;
    hints.ai_flags        = AI_NUMERICHOST;
    struct addrinfo *res  = NULL;
    int rc = nsgai_getaddrinfo("127.0.0.1", "http", &hints, &res);
    if (rc == 0) {
        MUST(((struct sockaddr_in *)res->ai_addr)->sin_port == htons(80));
        nsgai_freeaddrinfo(res);
    }

    /* SOCK_DGRAM branch (proto="udp"). */
    hints.ai_socktype = SOCK_DGRAM;
    res               = NULL;
    rc                = nsgai_getaddrinfo("127.0.0.1", "domain", &hints, &res);
    if (rc == 0) {
        nsgai_freeaddrinfo(res);
    }

    /* Numeric port that overflows uint16 -> EAI_SERVICE. */
    hints.ai_flags    = AI_NUMERICHOST | AI_NUMERICSERV;
    hints.ai_socktype = SOCK_STREAM;
    res               = NULL;
    rc                = nsgai_getaddrinfo("127.0.0.1", "999999", &hints, &res);
    MUST(rc == EAI_SERVICE);

    /* Unknown service name -> EAI_SERVICE. */
    hints.ai_flags = 0;
    res            = NULL;
    rc = nsgai_getaddrinfo("127.0.0.1", "nosuchservice-zzz", &hints, &res);
    MUST(rc != 0);

    return 0;
}

static int
test_canonname(void)
{
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_flags        = AI_CANONNAME;
    struct addrinfo *res  = NULL;
    int rc                = nsgai_getaddrinfo("localhost", NULL, &hints, &res);
    MUST(rc == 0);
    MUST(res != NULL);
    /* Canonical name is best-effort; just ensure no crash and at least
     * the head node has it set if c-ares provided one. */
    nsgai_freeaddrinfo(res);
    return 0;
}

static int
test_getnameinfo(void)
{
    /* IPv4 numeric host. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001);
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
    MUST(rc == 0);
    MUST(strcmp(host, "127.0.0.1") == 0);

    /* IPv6 numeric host (different code path inside c-ares). */
    struct sockaddr_in6 sa6;
    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    sa6.sin6_addr   = in6addr_loopback;
    rc = nsgai_getnameinfo((struct sockaddr *)&sa6, sizeof(sa6), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    MUST(rc == 0);

    /* NI_DGRAM flag (map_ni_flags branch). */
    rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), serv, sizeof(serv),
        NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM);
    MUST(rc == 0);

    /* NI_NOFQDN flag. */
    rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST | NI_NOFQDN);
    MUST(rc == 0);

    /* NI_NAMEREQD on an unlikely-resolvable address -> EAI_NONAME. */
    struct sockaddr_in sa_x;
    memset(&sa_x, 0, sizeof(sa_x));
    sa_x.sin_family = AF_INET;
    /* TEST-NET-1 (RFC 5737): must not be globally resolvable. */
    sa_x.sin_addr.s_addr = htonl(0xC0000201); /* 192.0.2.1 */
    rc = nsgai_getnameinfo((struct sockaddr *)&sa_x, sizeof(sa_x), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
    /* Either NONAME or AGAIN are acceptable - both exercise error code path. */
    MUST(rc != 0);

    return 0;
}

static int
test_explicit_input_validation(void)
{
    struct addrinfo *res = NULL;

    /* NULL ns_name. */
    int rc = nsgai_getaddrinfo_ns(NULL, "localhost", NULL, NULL, &res);
    MUST(rc == EAI_SYSTEM);
    MUST(errno == EINVAL);

    /* Negative fd. */
    rc = nsgai_getaddrinfo_nsfd(-1, "localhost", NULL, NULL, &res);
    MUST(rc == EAI_SYSTEM);
    MUST(errno == EINVAL);

    /* Non-existent ns name -> resolve_explicit_name fails. */
    rc =
        nsgai_getaddrinfo_ns("zz-no-such-ns-zz", "localhost", NULL, NULL, &res);
    MUST(rc != 0);

    /* Same for getnameinfo variants. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001);
    char host[64];

    rc = nsgai_getnameinfo_ns(NULL, (struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    MUST(rc == EAI_SYSTEM);

    rc = nsgai_getnameinfo_nsfd(-1, (struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    MUST(rc == EAI_SYSTEM);

    return 0;
}

static int
test_freeaddrinfo_null(void)
{
    /* Must be a no-op. */
    nsgai_freeaddrinfo(NULL);
    return 0;
}

static int
test_libc_aliases(void)
{
    /* When this program is run under LD_PRELOAD=libnsgai.so, the weak
     * libc-named aliases in libnsgai win over libc's strong symbols
     * and the test exercises them directly.  Without LD_PRELOAD libc
     * wins; we still call the symbols so the test compiles either way. */
    const char *m = gai_strerror(0);
    MUST(m && *m);

    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_flags        = AI_NUMERICHOST;
    struct addrinfo *r    = NULL;
    int rc                = getaddrinfo("127.0.0.1", NULL, &hints, &r);
    MUST(rc == 0 && r != NULL);
    freeaddrinfo(r);

    struct sockaddr_in sa = {0};
    sa.sin_family         = AF_INET;
    sa.sin_addr.s_addr    = htonl(0x7f000001);
    char host[64];
    rc = getnameinfo((struct sockaddr *)&sa, sizeof(sa), host, sizeof(host),
        NULL, 0, NI_NUMERICHOST);
    MUST(rc == 0);
    return 0;
}

static int
test_overflow(void)
{
    /* host buffer too small to hold "127.0.0.1\0" -> EAI_OVERFLOW. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001);
    char host[4];
    int rc = nsgai_getnameinfo((struct sockaddr *)&sa, sizeof(sa), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    MUST(rc == EAI_OVERFLOW);
    return 0;
}

int
main(void)
{
    if (test_gai_strerror()) {
        return 1;
    }
    if (test_numeric_host()) {
        return 1;
    }
    if (test_numeric_serv()) {
        return 1;
    }
    if (test_service_resolution()) {
        return 1;
    }
    if (test_canonname()) {
        return 1;
    }
    if (test_getnameinfo()) {
        return 1;
    }
    if (test_explicit_input_validation()) {
        return 1;
    }
    if (test_freeaddrinfo_null()) {
        return 1;
    }
    if (test_libc_aliases()) {
        return 1;
    }
    if (test_overflow()) {
        return 1;
    }
    printf("api coverage tests passed\n");
    return 0;
}
