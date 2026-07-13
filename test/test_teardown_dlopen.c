/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 *
 * Regression test for teardown after synchronous DNS-path lookups.
 *
 * This intentionally dlopen()s libnsgai instead of linking it directly.  Older
 * libnsgai builds could leave an idle cached c-ares channel alive after a
 * lookup.  With c-ares event-thread mode enabled, dlclose()/process teardown
 * could then leave a c-ares-owned thread with callbacks pointing into unloaded
 * libnsgai text, matching the rsyslog valgrind crash.
 */

#include "nsgai.h"

#include <dlfcn.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*nsgai_set_lookup_policy_fn)(nsgai_lookup_policy_t);
typedef int (*nsgai_getaddrinfo_fn)(
    const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef void (*nsgai_freeaddrinfo_fn)(struct addrinfo *);

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static void *
must_dlsym(void *handle, const char *name)
{
    dlerror();
    void *sym       = dlsym(handle, name);
    const char *err = dlerror();
    if (err != NULL || sym == NULL) {
        fprintf(stderr, "dlsym(%s): %s\n", name, err ? err : "not found");
        exit(2);
    }
    return sym;
}

static void
alarm_handler(int sig)
{
    (void)sig;
    _exit(124);
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/libnsgai.so\n", argv[0]);
        return 2;
    }

    signal(SIGALRM, alarm_handler);
    alarm(20);
    setenv("RES_OPTIONS", "attempts:1 timeout:1", 0);

    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen(%s): %s\n", argv[1], dlerror());
        return 2;
    }

    nsgai_set_lookup_policy_fn set_policy =
        (nsgai_set_lookup_policy_fn)must_dlsym(
            handle, "nsgai_set_lookup_policy");
    nsgai_getaddrinfo_fn lookup =
        (nsgai_getaddrinfo_fn)must_dlsym(handle, "nsgai_getaddrinfo");
    nsgai_freeaddrinfo_fn free_result =
        (nsgai_freeaddrinfo_fn)must_dlsym(handle, "nsgai_freeaddrinfo");

    CHECK(set_policy(NSGAI_LOOKUP_DNS_ONLY) == 0);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc               = lookup("localhost", "80", &hints, &res);
    if (res != NULL) {
        free_result(res);
    }
    fprintf(stderr, "DNS-only lookup returned %d\n", rc);

    CHECK(set_policy(NSGAI_LOOKUP_HOSTS_FIRST) == 0);

    if (dlclose(handle) != 0) {
        fprintf(stderr, "dlclose(%s): %s\n", argv[1], dlerror());
        return 2;
    }

    /* Give any stale event thread a chance to run after dlclose(). */
    usleep(250000);
    alarm(0);

    printf("teardown dlopen regression test passed\n");
    return 0;
}
