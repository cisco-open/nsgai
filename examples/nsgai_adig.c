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
 * nsgai_adig - a tiny `adig`-style command line tool that resolves
 * hostnames through libnsgai, optionally targeting a network namespace
 * by name (-n NS) or by an open file descriptor (-f FD).
 *
 * Usage:
 *   nsgai_adig [-4|-6] [-c] [-n NS_NAME | -f NS_FD] [-p POLICY] [-r]
 *              HOST [HOST...]
 *
 * Options:
 *   -4         IPv4 only (AF_INET)
 *   -6         IPv6 only (AF_INET6)
 *   -c         Request canonical name (AI_CANONNAME)
 *   -n NAME    Resolve in netns /var/run/netns/NAME
 *   -f FD      Resolve in the netns referred to by FD (must be open)
 *   -p POLICY  Set libnsgai lookup policy:
 *                hosts-first | dns-first | hosts-only | dns-only
 *   -r         Perform a reverse lookup on each argument
 *              (arguments must then be numeric IPv4/IPv6 addresses)
 *
 * Exit status: 0 on full success, 1 if any lookup failed.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
print_forward(const char *host, struct addrinfo *ai)
{
    int n = 0;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        const void *src            = NULL;
        if (p->ai_family == AF_INET) {
            src = &((struct sockaddr_in *)p->ai_addr)->sin_addr;
        } else if (p->ai_family == AF_INET6) {
            src = &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        if (!inet_ntop(p->ai_family, src, buf, sizeof(buf))) {
            continue;
        }
        printf("%-32s %s %s%s%s\n", host,
            (p->ai_family == AF_INET) ? "A   " : "AAAA", buf,
            p->ai_canonname ? "  # " : "", p->ai_canonname ?: "");
        n++;
    }
    if (n == 0) {
        printf("%-32s (no addresses)\n", host);
    }
}

static int
do_reverse(const char *arg, int ns_fd, const char *ns_name)
{
    struct sockaddr_storage ss;
    socklen_t slen;
    memset(&ss, 0, sizeof(ss));

    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, arg, &v4) == 1) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        sin->sin_family         = AF_INET;
        sin->sin_addr           = v4;
        slen                    = sizeof(*sin);
    } else if (inet_pton(AF_INET6, arg, &v6) == 1) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
        sin6->sin6_family         = AF_INET6;
        sin6->sin6_addr           = v6;
        slen                      = sizeof(*sin6);
    } else {
        fprintf(stderr, "nsgai_adig: not a numeric address: %s\n", arg);
        return -1;
    }

    char host[NI_MAXHOST] = {0};
    int rc;
    if (ns_fd >= 0) {
        rc = nsgai_getnameinfo_nsfd(ns_fd, (struct sockaddr *)&ss, slen, host,
            sizeof(host), NULL, 0, 0);
    } else if (ns_name) {
        rc = nsgai_getnameinfo_ns(ns_name, (struct sockaddr *)&ss, slen, host,
            sizeof(host), NULL, 0, 0);
    } else {
        rc = nsgai_getnameinfo(
            (struct sockaddr *)&ss, slen, host, sizeof(host), NULL, 0, 0);
    }

    if (rc != 0) {
        fprintf(stderr, "nsgai_adig: reverse %s failed: %s\n", arg,
            nsgai_gai_strerror(rc));
        return -1;
    }
    printf("%-40s PTR  %s\n", arg, host);
    return 0;
}

static void
usage(FILE *out)
{
    fprintf(out, "Usage: nsgai_adig [-4|-6] [-c] [-n NS_NAME | -f NS_FD] "
                 "[-p POLICY] [-r] HOST [HOST...]\n");
}

static int
parse_policy(const char *arg, nsgai_lookup_policy_t *policy)
{
    if (strcmp(arg, "hosts-first") == 0) {
        *policy = NSGAI_LOOKUP_HOSTS_FIRST;
        return 0;
    }
    if (strcmp(arg, "dns-first") == 0) {
        *policy = NSGAI_LOOKUP_DNS_FIRST;
        return 0;
    }
    if (strcmp(arg, "hosts-only") == 0) {
        *policy = NSGAI_LOOKUP_HOSTS_ONLY;
        return 0;
    }
    if (strcmp(arg, "dns-only") == 0) {
        *policy = NSGAI_LOOKUP_DNS_ONLY;
        return 0;
    }
    return -1;
}

int
main(int argc, char **argv)
{
    int family                   = AF_UNSPEC;
    int want_canon               = 0;
    int reverse                  = 0;
    const char *ns_name          = NULL;
    int ns_fd                    = -1;
    int have_policy              = 0;
    nsgai_lookup_policy_t policy = NSGAI_LOOKUP_HOSTS_FIRST;

    int opt;
    while ((opt = getopt(argc, argv, "46cn:f:p:rh")) != -1) {
        switch (opt) {
            case '4': family = AF_INET; break;
            case '6': family = AF_INET6; break;
            case 'c': want_canon = 1; break;
            case 'n': ns_name = optarg; break;
            case 'f':
                ns_fd = (int)strtol(optarg, NULL, 10);
                if (ns_fd < 0) {
                    fprintf(stderr, "nsgai_adig: bad fd: %s\n", optarg);
                    return 2;
                }
                break;
            case 'p':
                if (parse_policy(optarg, &policy) != 0) {
                    fprintf(stderr, "nsgai_adig: bad policy: %s\n", optarg);
                    return 2;
                }
                have_policy = 1;
                break;
            case 'r': reverse = 1; break;
            case 'h': usage(stdout); return 0;
            default: usage(stderr); return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }
    if (ns_name && ns_fd >= 0) {
        fprintf(stderr, "nsgai_adig: -n and -f are mutually exclusive\n");
        return 2;
    }
    if (have_policy && nsgai_set_lookup_policy(policy) != 0) {
        fprintf(stderr, "nsgai_adig: failed to set lookup policy\n");
        return 2;
    }

    int any_fail = 0;
    for (int i = optind; i < argc; i++) {
        const char *arg = argv[i];

        if (reverse) {
            if (do_reverse(arg, ns_fd, ns_name) != 0) {
                any_fail = 1;
            }
            continue;
        }

        struct addrinfo hints = {0};
        hints.ai_family       = family;
        hints.ai_socktype     = SOCK_STREAM;
        if (want_canon) {
            hints.ai_flags |= AI_CANONNAME;
        }

        struct addrinfo *res = NULL;
        int rc;
        if (ns_fd >= 0) {
            rc = nsgai_getaddrinfo_nsfd(ns_fd, arg, NULL, &hints, &res);
        } else if (ns_name) {
            rc = nsgai_getaddrinfo_ns(ns_name, arg, NULL, &hints, &res);
        } else {
            rc = nsgai_getaddrinfo(arg, NULL, &hints, &res);
        }

        if (rc != 0) {
            fprintf(
                stderr, "nsgai_adig: %s: %s\n", arg, nsgai_gai_strerror(rc));
            any_fail = 1;
            continue;
        }
        print_forward(arg, res);
        nsgai_freeaddrinfo(res);
    }
    return any_fail ? 1 : 0;
}
