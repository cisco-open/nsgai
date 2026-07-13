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
 * Coverage-focused unit test for libnss_nsgai.so.2.
 *
 * Loads the NSS module by absolute path (the path is passed on the
 * command line, supplied by CMake at test time), dlsym's each of its
 * exported `_nss_nsgai_*` entry points, and exercises both their
 * success paths and the documented error paths (ERANGE/TRYAGAIN,
 * EAFNOSUPPORT, NOTFOUND, EINVAL).
 *
 * This test does NOT require root or a netns - everything resolves via
 * the implicit current-ns path, which on a normal system finds
 * `localhost` in /etc/hosts via c-ares' hosts-file lookup.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <nss.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef enum nss_status (*ghbn_r_t)(const char *name,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop);
typedef enum nss_status (*ghbn2_r_t)(const char *name,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop);
typedef enum nss_status (*ghbn4_r_t)(const char *name,
    struct gaih_addrtuple **pat,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop,
    int32_t *ttlp);
typedef enum nss_status (*ghba_r_t)(const void *addr,
    socklen_t len,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop);
typedef enum nss_status (*ghba2_r_t)(const void *addr,
    socklen_t len,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop,
    int32_t *ttlp);

#define MUST(expr)                                                             \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void *
load_sym(void *h, const char *name)
{
    void *p = dlsym(h, name);
    if (!p) {
        fprintf(stderr, "dlsym(%s): %s\n", name, dlerror());
        exit(1);
    }
    return p;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/libnss_nsgai.so.2\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen(%s): %s\n", argv[1], dlerror());
        return 1;
    }

    ghbn_r_t ghbn   = (ghbn_r_t)load_sym(h, "_nss_nsgai_gethostbyname_r");
    ghbn2_r_t ghbn2 = (ghbn2_r_t)load_sym(h, "_nss_nsgai_gethostbyname2_r");
    ghbn4_r_t ghbn4 = (ghbn4_r_t)load_sym(h, "_nss_nsgai_gethostbyname4_r");
    ghba_r_t ghba   = (ghba_r_t)load_sym(h, "_nss_nsgai_gethostbyaddr_r");
    ghba2_r_t ghba2 = (ghba2_r_t)load_sym(h, "_nss_nsgai_gethostbyaddr2_r");

    enum nss_status st;
    int err, herr;
    struct hostent he;
    char buf[4096];

    /* ----- gethostbyname_r: success (forwards to gethostbyname2_r) ----- */
    err = herr = 0;
    memset(&he, 0, sizeof(he));
    st = ghbn("127.0.0.1", &he, buf, sizeof(buf), &err, &herr);
    MUST(st == NSS_STATUS_SUCCESS);
    MUST(he.h_addrtype == AF_INET);
    MUST(he.h_length == 4);
    MUST(he.h_addr_list && he.h_addr_list[0]);

    /* ----- gethostbyname2_r: AF_INET6 (may NOTFOUND in minimal env) ---- */
    err = herr = 0;
    memset(&he, 0, sizeof(he));
    st = ghbn2("::1", AF_INET6, &he, buf, sizeof(buf), &err, &herr);
    MUST(st == NSS_STATUS_SUCCESS || st == NSS_STATUS_NOTFOUND);
    if (st == NSS_STATUS_SUCCESS) {
        MUST(he.h_addrtype == AF_INET6);
    }

    /* ----- gethostbyname2_r: bad family (EAFNOSUPPORT branch) --------- */
    err = herr = 0;
    st = ghbn2("localhost", AF_UNIX, &he, buf, sizeof(buf), &err, &herr);
    MUST(st == NSS_STATUS_UNAVAIL);
    MUST(err == EAFNOSUPPORT);

    /* ----- gethostbyname2_r: not-found (HOST_NOT_FOUND branch) -------- */
    err = herr = 0;
    st = ghbn2("bad host name", AF_INET, &he, buf, sizeof(buf), &err, &herr);
    MUST(st == NSS_STATUS_NOTFOUND || st == NSS_STATUS_UNAVAIL ||
         st == NSS_STATUS_TRYAGAIN);

    /* ----- gethostbyname2_r: tiny buffer (ERANGE/TRYAGAIN branch) ----- */
    {
        char tiny[8];
        err = herr = 0;
        st = ghbn2("127.0.0.1", AF_INET, &he, tiny, sizeof(tiny), &err, &herr);
        MUST(st == NSS_STATUS_TRYAGAIN);
        MUST(err == ERANGE);
    }

    /* ----- gethostbyname4_r: success + ttl plumbing ------------------ */
    {
        struct gaih_addrtuple *at = NULL;
        int32_t ttl               = -1;
        err = herr = 0;
        st = ghbn4("127.0.0.1", &at, buf, sizeof(buf), &err, &herr, &ttl);
        MUST(st == NSS_STATUS_SUCCESS);
        MUST(at != NULL);
        MUST(at->family == AF_INET || at->family == AF_INET6);
        MUST(ttl >= 0); /* either 0 (no ttl) or a positive value */
    }

    /* ----- gethostbyname4_r: tiny buffer ----------------------------- */
    {
        struct gaih_addrtuple *at = NULL;
        char tiny[8];
        err = herr = 0;
        st = ghbn4("127.0.0.1", &at, tiny, sizeof(tiny), &err, &herr, NULL);
        MUST(st == NSS_STATUS_TRYAGAIN);
        MUST(err == ERANGE);
    }

    /* ----- gethostbyaddr_r: bad len/af combo (EINVAL branch) --------- */
    {
        uint8_t v4[4] = {127, 0, 0, 1};
        err = herr = 0;
        /* len=16 but af=AF_INET -> EINVAL/UNAVAIL */
        st = ghba(v4, 16, AF_INET, &he, buf, sizeof(buf), &err, &herr);
        MUST(st == NSS_STATUS_UNAVAIL);
        MUST(err == EINVAL);
    }

    /* ----- gethostbyaddr2_r: TTL slot is zeroed on error too ---------- */
    {
        uint8_t v4[4] = {127, 0, 0, 1};
        int32_t ttl   = 0x1234;
        err = herr = 0;
        /* len=16 but af=AF_INET -> deterministic EINVAL/UNAVAIL */
        st = ghba2(v4, 16, AF_INET, &he, buf, sizeof(buf), &err, &herr, &ttl);
        MUST(st == NSS_STATUS_UNAVAIL);
        MUST(err == EINVAL);
        MUST(ttl == 0); /* gethostbyaddr2_r always reports ttl=0 */
    }

    /*
     * Keep the NSS module loaded until process exit.  Unloading here can
     * race with c-ares event-thread teardown and intermittently fault.
     */
    printf("nss tests passed\n");
    return 0;
}
