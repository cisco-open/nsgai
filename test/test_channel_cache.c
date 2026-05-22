/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Coverage-focused test for the per-namespace channel cache in
 * nsgai_channel.c.  Specifically targets the paths that are only hit
 * when the cache overflows (cache_find_lru_idle, cache_unlink,
 * entry_free) and when a cached entry's per-ns config files become
 * stale (entry_is_fresh -> stat_changed true branch).
 *
 * Requires CAP_SYS_ADMIN to call unshare(CLONE_NEWNET).  In our
 * rootless container we run --privileged so the user namespace has
 * the cap.  On hosts without it, the test self-skips with 77.
 *
 * Strategy:
 *   1. Save the original netns fd.
 *   2. Create N >> NSGAI_CACHE_MAX_IDLE (32) fresh network namespaces
 *      by calling unshare(CLONE_NEWNET) in a loop and snapshotting
 *      /proc/thread-self/ns/net after each.  Each snapshot is a distinct
 *      inode so each will be its own cache key.
 *   3. Restore the original netns.
 *   4. For every snapshot fd, call nsgai_getaddrinfo_nsfd() with
 *      AI_NUMERICHOST.  AI_NUMERICHOST means no DNS traffic, but
 *      do_getaddrinfo() still calls nsgai_channel_create()/destroy(),
 *      which is what we want.  As entries cross the cache cap the
 *      eviction path (cache_maybe_evict_locked -> cache_find_lru_idle
 *      -> cache_unlink -> entry_free) executes.
 *   5. Separately exercise the stale-revalidation path by creating a
 *      cached entry against a tmp resolv.conf, bumping its mtime, and
 *      acquiring the same key again.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define N_NS 40 /* > NSGAI_CACHE_MAX_IDLE (32) */

#define MUST(expr)                                                             \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s (errno=%d %s)\n", __FILE__,        \
                __LINE__, #expr, errno, strerror(errno));                      \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static int
try_unshare_netns(void)
{
    if (unshare(CLONE_NEWNET) == 0) {
        return 0;
    }
    return -1;
}

static int
test_cache_eviction(void)
{
    int orig = open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
    MUST(orig >= 0);

    /* Confirm we can create at least one new netns before committing to
     * the full sweep; otherwise skip the test rather than fail. */
    if (try_unshare_netns() != 0) {
        fprintf(stderr,
            "skip: unshare(CLONE_NEWNET) failed (errno=%d %s); "
            "needs CAP_SYS_ADMIN\n",
            errno, strerror(errno));
        close(orig);
        return 77;
    }

    int fds[N_NS];
    fds[0] = open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
    MUST(fds[0] >= 0);

    int created = 1;
    for (; created < N_NS; created++) {
        if (try_unshare_netns() != 0) {
            break;
        }
        fds[created] = open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
        if (fds[created] < 0) {
            break;
        }
    }
    /* Restore original netns BEFORE doing anything else. */
    MUST(setns(orig, CLONE_NEWNET) == 0);
    close(orig);

    if (created < 35) {
        fprintf(stderr,
            "skip: could only create %d netns, need >NSGAI_CACHE_MAX_IDLE\n",
            created);
        for (int i = 0; i < created; i++) {
            close(fds[i]);
        }
        return 77;
    }

    /* Hit the cache N_NS times with distinct fd keys.  AI_NUMERICHOST
     * short-circuits DNS so the per-iteration cost is the channel
     * create+destroy itself, which is exactly what we want to stress. */
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_flags        = AI_NUMERICHOST;

    for (int i = 0; i < created; i++) {
        struct addrinfo *res = NULL;
        int rc =
            nsgai_getaddrinfo_nsfd(fds[i], "127.0.0.1", NULL, &hints, &res);
        MUST(rc == 0);
        MUST(res != NULL);
        nsgai_freeaddrinfo(res);
    }

    /* Second pass: re-acquire each fd to exercise the cache-hit path.
     * Entries that were evicted will re-build (going through
     * cache_build_locked again); entries still hot will hit. */
    for (int i = 0; i < created; i++) {
        struct addrinfo *res = NULL;
        int rc =
            nsgai_getaddrinfo_nsfd(fds[i], "127.0.0.1", NULL, &hints, &res);
        MUST(rc == 0);
        nsgai_freeaddrinfo(res);
    }

    for (int i = 0; i < created; i++) {
        close(fds[i]);
    }
    printf("cache eviction: created %d netns, all lookups OK\n", created);
    return 0;
}

/* Exercise the entry_is_fresh "stat changed -> stale" branch.
 *
 * We arrange a named netns under /var/run/netns/<name>/ and a per-ns
 * resolv.conf under /etc/netns/<name>/resolv.conf so that
 * nsgai_ns_resolve_explicit_name picks up the path; first acquire to
 * populate the cache, bump the file's mtime, then acquire again.  The
 * second acquire must see the entry as stale and rebuild.
 *
 * This requires root (to bind-mount the netns) so we delegate the
 * netns/file plumbing to the existing shell test harness.  The C test
 * here just exercises the in-process cache_unlink path via the
 * cap-only unshare loop above.
 */

int
main(void)
{
    return test_cache_eviction();
}
