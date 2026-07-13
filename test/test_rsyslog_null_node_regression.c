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
 * Regression test for rsyslog-style getaddrinfo() usage:
 *   - node == NULL
 *   - AI_NUMERICSERV (+ AI_PASSIVE for server sockets)
 *   - SOCK_DGRAM
 *
 * This test guards against reintroducing crashes in the node==NULL path.
 */

#include "nsgai.h"

#include <arpa/inet.h>
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
check_v4_addr(const struct addrinfo *ai, uint32_t expect_s_addr_n)
{
    for (const struct addrinfo *p = ai; p; p = p->ai_next) {
        if (p->ai_family != AF_INET) {
            continue;
        }
        const struct sockaddr_in *sin = (const struct sockaddr_in *)p->ai_addr;
        if (sin->sin_addr.s_addr == expect_s_addr_n) {
            return 1;
        }
    }
    return 0;
}

static int
check_v6_addr(const struct addrinfo *ai, const struct in6_addr *expect)
{
    for (const struct addrinfo *p = ai; p; p = p->ai_next) {
        if (p->ai_family != AF_INET6) {
            continue;
        }
        const struct sockaddr_in6 *sin6 =
            (const struct sockaddr_in6 *)p->ai_addr;
        if (memcmp(&sin6->sin6_addr, expect, sizeof(*expect)) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
run_null_node_case(int family, int passive)
{
    struct addrinfo hints = {0};
    hints.ai_flags        = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    hints.ai_family       = family;
    hints.ai_socktype     = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    int rc               = nsgai_getaddrinfo(NULL, "514", &hints, &res);
    MUST(rc == 0);
    MUST(res != NULL);

    for (const struct addrinfo *p = res; p; p = p->ai_next) {
        MUST(p->ai_socktype == SOCK_DGRAM);
        if (p->ai_family == AF_INET) {
            const struct sockaddr_in *sin =
                (const struct sockaddr_in *)p->ai_addr;
            MUST(sin->sin_port == htons(514));
        } else if (p->ai_family == AF_INET6) {
            const struct sockaddr_in6 *sin6 =
                (const struct sockaddr_in6 *)p->ai_addr;
            MUST(sin6->sin6_port == htons(514));
        }
    }

    if (family == AF_INET || family == AF_UNSPEC) {
        uint32_t want = htonl(passive ? INADDR_ANY : INADDR_LOOPBACK);
        MUST(check_v4_addr(res, want));
    }
    if (family == AF_INET6 || family == AF_UNSPEC) {
        const struct in6_addr *want =
            passive ? &in6addr_any : &in6addr_loopback;
        MUST(check_v6_addr(res, want));
    }

    nsgai_freeaddrinfo(res);
    return 0;
}

static int
run_client_numeric_host_case(void)
{
    struct addrinfo hints = {0};
    hints.ai_flags        = AI_NUMERICSERV;
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    int rc               = nsgai_getaddrinfo("127.0.0.1", "514", &hints, &res);
    MUST(rc == 0);
    MUST(res != NULL);
    nsgai_freeaddrinfo(res);
    return 0;
}

int
main(void)
{
    /* Tight loop to catch crash regressions in repeated startup/config paths. */
    for (int i = 0; i < 5000; i++) {
        if (run_null_node_case(AF_UNSPEC, 1)) {
            return 1;
        }
        if (run_null_node_case(AF_UNSPEC, 0)) {
            return 1;
        }
        if (run_null_node_case(AF_INET, 1)) {
            return 1;
        }
        if (run_null_node_case(AF_INET6, 0)) {
            return 1;
        }
        if (run_client_numeric_host_case()) {
            return 1;
        }
    }

    printf("rsyslog null-node regression test passed\n");
    return 0;
}
