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
 * test_multins - exercise nsgai across multiple network namespaces from
 * multiple threads concurrently.
 *
 * Usage:
 *   test_multins HOST NS1=ADDR1 [NS2=ADDR2 ...] [--iterations N]
 *
 * Each NSk=ADDRk pair becomes one worker thread that:
 *   - calls nsgai_getaddrinfo_ns("NSk", HOST, ...) repeatedly
 *   - verifies each result is exactly ADDRk
 *
 * Exits non-zero on any mismatch, lookup error, or thread error.
 *
 * The threads share one process but each targets its own netns via the
 * explicit _ns entry point. The c-ares socket callback enters/leaves
 * the netns per-call (on its own event thread) without disturbing the
 * caller's thread or its siblings.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct {
    const char *host;
    const char *ns;
    const char *expected_v4; /* presentation form */
    int iterations;
    int ok; /* 0 success, !=0 failure */
    char errmsg[256];
} job_t;

static void
set_err(job_t *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->errmsg, sizeof(j->errmsg), fmt, ap);
    va_end(ap);
    j->ok = 1;
}

static void *
worker(void *arg)
{
    job_t *j = (job_t *)arg;
    j->ok    = 0;

    struct in_addr want;
    if (inet_pton(AF_INET, j->expected_v4, &want) != 1) {
        set_err(j, "bad expected v4 '%s'", j->expected_v4);
        return NULL;
    }

    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;

    for (int i = 0; i < j->iterations; i++) {
        struct addrinfo *res = NULL;
        int rc = nsgai_getaddrinfo_ns(j->ns, j->host, NULL, &hints, &res);
        if (rc != 0) {
            set_err(j, "iter %d: getaddrinfo(%s in %s) failed: %s", i, j->host,
                j->ns, nsgai_gai_strerror(rc));
            return NULL;
        }
        int matched = 0;
        for (struct addrinfo *p = res; p; p = p->ai_next) {
            if (p->ai_family != AF_INET) {
                continue;
            }
            struct in_addr got = ((struct sockaddr_in *)p->ai_addr)->sin_addr;
            if (got.s_addr == want.s_addr) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            char buf[INET_ADDRSTRLEN] = "?";
            if (res && res->ai_family == AF_INET) {
                inet_ntop(AF_INET,
                    &((struct sockaddr_in *)res->ai_addr)->sin_addr, buf,
                    sizeof(buf));
            }
            set_err(j, "iter %d in %s: expected %s, got %s", i, j->ns,
                j->expected_v4, buf);
            nsgai_freeaddrinfo(res);
            return NULL;
        }
        nsgai_freeaddrinfo(res);
    }
    return NULL;
}

static int
parse_pair(char *arg, const char **ns, const char **addr)
{
    char *eq = strchr(arg, '=');
    if (!eq) {
        return -1;
    }
    *eq   = '\0';
    *ns   = arg;
    *addr = eq + 1;
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s HOST NS=ADDR [NS=ADDR ...] [--iterations N]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    int iterations   = 20;

    /* Pre-pass for --iterations N */
    int npairs = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = atoi(argv[i + 1]);
            i++;
            continue;
        }
        npairs++;
    }
    if (npairs == 0) {
        fprintf(stderr, "%s: need at least one NS=ADDR pair\n", argv[0]);
        return 2;
    }

    job_t *jobs     = (job_t *)calloc((size_t)npairs, sizeof(*jobs));
    pthread_t *tids = (pthread_t *)calloc((size_t)npairs, sizeof(*tids));
    if (!jobs || !tids) {
        perror("calloc");
        return 2;
    }

    int k = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0) {
            i++;
            continue;
        }
        if (parse_pair(argv[i], &jobs[k].ns, &jobs[k].expected_v4) != 0) {
            fprintf(stderr, "bad arg '%s', need NS=ADDR\n", argv[i]);
            return 2;
        }
        jobs[k].host       = host;
        jobs[k].iterations = iterations;
        k++;
    }

    for (int i = 0; i < npairs; i++) {
        int rc = pthread_create(&tids[i], NULL, worker, &jobs[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return 2;
        }
    }

    int fails = 0;
    for (int i = 0; i < npairs; i++) {
        pthread_join(tids[i], NULL);
        if (jobs[i].ok != 0) {
            fprintf(stderr, "FAIL ns=%s: %s\n", jobs[i].ns, jobs[i].errmsg);
            fails++;
        } else {
            fprintf(stdout, "PASS ns=%s host=%s addr=%s iters=%d\n", jobs[i].ns,
                host, jobs[i].expected_v4, iterations);
        }
    }

    free(jobs);
    free(tids);
    return fails ? 1 : 0;
}
