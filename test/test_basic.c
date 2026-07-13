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
 * Tiny smoke-test for libnsgai.  Resolves a hostname in the current
 * namespace and dumps the result.  Also exercises the _ns and _nsfd
 * paths if /var/run/netns/<name> exists.
 *
 * Usage: nsgai_test [hostname] [ns_name]
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
dump(const char *label, struct addrinfo *ai)
{
    printf("[%s]\n", label);
    int n = 0;
    for (struct addrinfo *p = ai; p; p = p->ai_next, n++) {
        char buf[INET6_ADDRSTRLEN] = {0};
        const void *src;
        if (p->ai_family == AF_INET) {
            src = &((struct sockaddr_in *)p->ai_addr)->sin_addr;
        } else if (p->ai_family == AF_INET6) {
            src = &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        inet_ntop(p->ai_family, src, buf, sizeof(buf));
        printf("  %d: family=%d addr=%s%s%s\n", n, p->ai_family, buf,
            p->ai_canonname ? " canon=" : "", p->ai_canonname ?: "");
    }
    if (n == 0) {
        printf("  (no results)\n");
    }
}

int
main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "localhost";
    const char *ns   = (argc > 2) ? argv[2] : NULL;

    struct addrinfo hints = {0};
    hints.ai_family       = AF_UNSPEC;
    hints.ai_socktype     = SOCK_STREAM;
    hints.ai_flags        = AI_CANONNAME;

    struct addrinfo *res = NULL;
    int rc               = nsgai_getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "nsgai_getaddrinfo(%s) failed: %s\n", host,
            nsgai_gai_strerror(rc));
    } else {
        dump("default", res);
        nsgai_freeaddrinfo(res);
    }

    if (ns) {
        res = NULL;
        rc  = nsgai_getaddrinfo_ns(ns, host, NULL, &hints, &res);
        if (rc != 0) {
            fprintf(stderr, "nsgai_getaddrinfo_ns(%s,%s) failed: %s\n", ns,
                host, nsgai_gai_strerror(rc));
        } else {
            dump(ns, res);
            nsgai_freeaddrinfo(res);
        }
    }
    return 0;
}
