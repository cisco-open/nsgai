/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Coverage-focused test for the cache freshness re-validation path in
 * nsgai_channel.c (entry_is_fresh stat-changed branch, plus the
 * "victim" handling inside nsgai_channel_create and the stale-on-
 * release branch of nsgai_channel_destroy).
 *
 * Usage: test_cache_stale <ns_name> <expected_addr>
 *
 * Assumes /var/run/netns/<ns_name> exists and
 * /etc/netns/<ns_name>/resolv.conf points at a DNS server that resolves
 * the hardcoded hostname "shared.nsgai.test" to <expected_addr>.  This
 * matches the layout produced by test/integration_dns_netns.sh.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define HOST "shared.nsgai.test"

static int
has_addr(struct addrinfo *res, const char *expected)
{
    char buf[INET6_ADDRSTRLEN];
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        const void *src;
        if (p->ai_family == AF_INET) {
            src = &((struct sockaddr_in *)p->ai_addr)->sin_addr;
        } else if (p->ai_family == AF_INET6) {
            src = &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        if (inet_ntop(p->ai_family, src, buf, sizeof(buf)) &&
            strcmp(buf, expected) == 0) {
            return 1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ns_name> <expected_addr>\n", argv[0]);
        return 2;
    }
    const char *ns_name  = argv[1];
    const char *expected = argv[2];

    char rc_path[256];
    snprintf(rc_path, sizeof(rc_path), "/etc/netns/%s/resolv.conf", ns_name);

    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;

    /* Round 1: cold cache - cache_build_locked. */
    struct addrinfo *res1 = NULL;
    int rc = nsgai_getaddrinfo_ns(ns_name, HOST, NULL, &hints, &res1);
    if (rc != 0) {
        fprintf(stderr, "round1 nsgai_getaddrinfo_ns(%s): %s\n", ns_name,
            nsgai_gai_strerror(rc));
        return 1;
    }
    if (!has_addr(res1, expected)) {
        fprintf(stderr, "round1: did not see %s in results\n", expected);
        nsgai_freeaddrinfo(res1);
        return 1;
    }
    nsgai_freeaddrinfo(res1);

    /* Round 2: warm cache - should be a cache hit (no stale). */
    struct addrinfo *res2 = NULL;
    rc = nsgai_getaddrinfo_ns(ns_name, HOST, NULL, &hints, &res2);
    if (rc != 0) {
        fprintf(stderr, "round2 nsgai_getaddrinfo_ns(%s): %s\n", ns_name,
            nsgai_gai_strerror(rc));
        return 1;
    }
    nsgai_freeaddrinfo(res2);

    /* Bump resolv.conf mtime to force the cached entry to be detected
     * as stale on the next acquire.  Use a deliberately advanced time
     * to dodge any 1-second filesystem timestamp granularity. */
    struct stat st;
    if (stat(rc_path, &st) != 0) {
        fprintf(stderr, "stat(%s): %s\n", rc_path, strerror(errno));
        return 1;
    }
    struct utimbuf ut;
    ut.actime  = st.st_atime;
    ut.modtime = st.st_mtime + 5;
    if (utime(rc_path, &ut) != 0) {
        fprintf(stderr, "utime(%s): %s\n", rc_path, strerror(errno));
        return 1;
    }

    /* Round 3: cache should detect staleness, drop the entry, and
     * rebuild.  This is the path that exercises entry_is_fresh's
     * stat_changed=true branch, nsgai_channel_create's victim handling,
     * cache_unlink, and entry_free. */
    struct addrinfo *res3 = NULL;
    rc = nsgai_getaddrinfo_ns(ns_name, HOST, NULL, &hints, &res3);
    if (rc != 0) {
        fprintf(stderr, "round3 (post-touch) nsgai_getaddrinfo_ns(%s): %s\n",
            ns_name, nsgai_gai_strerror(rc));
        return 1;
    }
    if (!has_addr(res3, expected)) {
        fprintf(stderr, "round3: did not see %s in results\n", expected);
        nsgai_freeaddrinfo(res3);
        return 1;
    }
    nsgai_freeaddrinfo(res3);

    printf(
        "cache stale: 3 rounds against ns=%s addr=%s OK\n", ns_name, expected);
    return 0;
}
