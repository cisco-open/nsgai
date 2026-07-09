/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MUST(expr)                                                             \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static volatile int g_thread_notified          = 0;
static volatile sig_atomic_t g_signal_notified = 0;

static void
notify_done(union sigval value)
{
    (void)value;
    g_thread_notified = 1;
}

static void
notify_signal(int signo)
{
    (void)signo;
    g_signal_notified = 1;
}

static int
wait_for_completion(const struct gaicb *const *list, int ent, struct gaicb *req)
{
    struct timespec timeout;
    int rc;
    int spins = 0;

    timeout.tv_sec  = 1;
    timeout.tv_nsec = 0;
    do {
        rc = nsgai_suspend(list, ent, &timeout);
    } while (rc == EAI_AGAIN &&
             ((req != NULL && nsgai_error(req) == EAI_INPROGRESS) ||
                 (req == NULL && spins++ < 10)));

    return rc;
}

static int
test_wait_mode(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    MUST(nsgai_getaddrinfo_a(GAI_WAIT, list, 1, NULL) == 0);
    MUST(nsgai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    MUST(nsgai_cancel(&req) == EAI_ALLDONE);
    return 0;
}

static int
test_nowait_suspend_and_error(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;
    int rc;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(nsgai_error(&req) == EAI_INPROGRESS || nsgai_error(&req) == 0);

    rc = wait_for_completion((const struct gaicb *const *)list, 1, &req);
    MUST(rc == 0);
    MUST(nsgai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    return 0;
}

static int
test_thread_notification(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;
    struct sigevent sev;
    int spins = 0;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    memset(&sev, 0, sizeof(sev));
    g_thread_notified = 0;

    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    sev.sigev_notify          = SIGEV_THREAD;
    sev.sigev_notify_function = notify_done;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, &sev) == 0);
    while (!g_thread_notified && spins++ < 100) {
        if (nsgai_error(&req) != EAI_INPROGRESS) {
            usleep(10000);
        } else {
            usleep(10000);
        }
    }

    MUST(g_thread_notified == 1);
    MUST(nsgai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    return 0;
}

static int
test_cancel_race(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;
    int cancel_rc;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == 0);
    cancel_rc = nsgai_cancel(&req);
    MUST(cancel_rc == EAI_CANCELED || cancel_rc == EAI_NOTCANCELED ||
         cancel_rc == EAI_ALLDONE);

    if (cancel_rc == EAI_CANCELED) {
        MUST(nsgai_error(&req) == EAI_CANCELED);
        MUST(req.ar_result == NULL);
        return 0;
    }

    if (nsgai_error(&req) == EAI_INPROGRESS) {
        MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req) ==
             0);
    }

    MUST(nsgai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    return 0;
}

static int
test_bad_mode(void)
{
    struct gaicb req;
    struct gaicb *list[1];

    memset(&req, 0, sizeof(req));
    list[0] = &req;
    MUST(nsgai_getaddrinfo_a(99, list, 1, NULL) == EAI_SYSTEM);
    return 0;
}

static int
test_ns_aliases(void)
{
    struct gaicb req1, req2;
    struct gaicb *list1[1], *list2[1];
    struct addrinfo hints;

    memset(&req1, 0, sizeof(req1));
    memset(&req2, 0, sizeof(req2));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;

    req1.ar_name    = "127.0.0.1";
    req1.ar_request = &hints;
    list1[0]        = &req1;
    MUST(nsgai_getaddrinfo_a_ns(NULL, GAI_WAIT, list1, 1, NULL) == 0);
    MUST(nsgai_error(&req1) == 0);
    MUST(req1.ar_result != NULL);
    nsgai_freeaddrinfo(req1.ar_result);

    req2.ar_name    = "127.0.0.1";
    req2.ar_request = &hints;
    list2[0]        = &req2;
    MUST(nsgai_getaddrinfo_a_nsfd(-1, GAI_WAIT, list2, 1, NULL) == 0);
    MUST(nsgai_error(&req2) == 0);
    MUST(req2.ar_result != NULL);
    nsgai_freeaddrinfo(req2.ar_result);

    return 0;
}

static int
test_multi_request_array(void)
{
    struct gaicb req1, req2;
    struct gaicb *list[3];
    struct addrinfo hints1, hints2;
    int complete1 = 0;
    int complete2 = 0;

    memset(&req1, 0, sizeof(req1));
    memset(&req2, 0, sizeof(req2));
    memset(&hints1, 0, sizeof(hints1));
    memset(&hints2, 0, sizeof(hints2));

    hints1.ai_family = AF_INET;
    hints1.ai_flags  = AI_NUMERICHOST;
    req1.ar_name     = "127.0.0.1";
    req1.ar_request  = &hints1;

    hints2.ai_family = AF_INET;
    req2.ar_name     = "localhost";
    req2.ar_request  = &hints2;

    list[0] = &req1;
    list[1] = NULL;
    list[2] = &req2;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 3, NULL) == 0);

    for (int i = 0; i < 10 && (!complete1 || !complete2); ++i) {
        int rc =
            wait_for_completion((const struct gaicb *const *)list, 3, NULL);
        MUST(rc == 0 || rc == EAI_ALLDONE);

        if (!complete1 && nsgai_error(&req1) != EAI_INPROGRESS) {
            MUST(nsgai_error(&req1) == 0);
            MUST(req1.ar_result != NULL);
            complete1 = 1;
        }
        if (!complete2 && nsgai_error(&req2) != EAI_INPROGRESS) {
            MUST(nsgai_error(&req2) == 0);
            MUST(req2.ar_result != NULL);
            complete2 = 1;
        }
    }

    MUST(complete1 == 1);
    MUST(complete2 == 1);
    nsgai_freeaddrinfo(req1.ar_result);
    nsgai_freeaddrinfo(req2.ar_result);
    return 0;
}

static int
test_signal_notification(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;
    struct sigevent sev;
    struct sigaction oldact, act;
    int spins = 0;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    memset(&sev, 0, sizeof(sev));
    memset(&act, 0, sizeof(act));
    g_signal_notified = 0;

    act.sa_handler = notify_signal;
    MUST(sigemptyset(&act.sa_mask) == 0);
    MUST(sigaction(SIGUSR1, &act, &oldact) == 0);

    hints.ai_family = AF_INET;
    req.ar_name     = "localhost";
    req.ar_request  = &hints;
    list[0]         = &req;

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo  = SIGUSR1;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, &sev) == 0);
    while (!g_signal_notified && spins++ < 200) {
        usleep(10000);
    }

    MUST(sigaction(SIGUSR1, &oldact, NULL) == 0);
    MUST(g_signal_notified == 1);
    MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req) == 0);
    MUST(nsgai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    return 0;
}

static int
test_cancel_all(void)
{
    struct gaicb req1, req2;
    struct gaicb *list[2];
    struct addrinfo hints1, hints2;
    int cancel_rc;

    memset(&req1, 0, sizeof(req1));
    memset(&req2, 0, sizeof(req2));
    memset(&hints1, 0, sizeof(hints1));
    memset(&hints2, 0, sizeof(hints2));

    hints1.ai_family = AF_INET;
    req1.ar_name     = "localhost";
    req1.ar_request  = &hints1;

    hints2.ai_family = AF_INET;
    hints2.ai_flags  = AI_NUMERICHOST;
    req2.ar_name     = "127.0.0.1";
    req2.ar_request  = &hints2;

    list[0] = &req1;
    list[1] = &req2;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 2, NULL) == 0);
    cancel_rc = nsgai_cancel(NULL);
    MUST(cancel_rc == EAI_CANCELED || cancel_rc == EAI_NOTCANCELED ||
         cancel_rc == EAI_ALLDONE);

    MUST(nsgai_error(&req1) != EAI_INPROGRESS);
    MUST(nsgai_error(&req2) != EAI_INPROGRESS);

    if (nsgai_error(&req1) == 0) {
        MUST(req1.ar_result != NULL);
        nsgai_freeaddrinfo(req1.ar_result);
    } else {
        MUST(nsgai_error(&req1) == EAI_CANCELED);
        MUST(req1.ar_result == NULL);
    }

    if (nsgai_error(&req2) == 0) {
        MUST(req2.ar_result != NULL);
        nsgai_freeaddrinfo(req2.ar_result);
    } else {
        MUST(nsgai_error(&req2) == EAI_CANCELED);
        MUST(req2.ar_result == NULL);
    }

    return 0;
}

static int
test_deterministic_error_paths(void)
{
    struct gaicb req_wait, req_nowait;
    struct gaicb *list[1];
    struct addrinfo hints_wait, hints_nowait;

    memset(&req_wait, 0, sizeof(req_wait));
    memset(&req_nowait, 0, sizeof(req_nowait));
    memset(&hints_wait, 0, sizeof(hints_wait));
    memset(&hints_nowait, 0, sizeof(hints_nowait));

    hints_wait.ai_family = AF_INET;
    hints_wait.ai_flags  = AI_NUMERICHOST;
    req_wait.ar_name     = "not-an-ip";
    req_wait.ar_request  = &hints_wait;
    list[0]              = &req_wait;

    MUST(nsgai_getaddrinfo_a(GAI_WAIT, list, 1, NULL) == 0);
    MUST(nsgai_error(&req_wait) != 0);
    MUST(req_wait.ar_result == NULL);

    hints_nowait.ai_family = AF_INET;
    hints_nowait.ai_flags  = AI_NUMERICHOST;
    req_nowait.ar_name     = "still-not-an-ip";
    req_nowait.ar_request  = &hints_nowait;
    list[0]                = &req_nowait;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(wait_for_completion(
             (const struct gaicb *const *)list, 1, &req_nowait) == 0);
    MUST(nsgai_error(&req_nowait) != 0);
    MUST(req_nowait.ar_result == NULL);
    return 0;
}

static int
test_libc_aliases(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    MUST(getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req) == 0);
    MUST(gai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    MUST(gai_cancel(&req) == EAI_ALLDONE);
    return 0;
}

int
main(void)
{
    if (test_wait_mode()) {
        return 1;
    }
    if (test_nowait_suspend_and_error()) {
        return 1;
    }
    if (test_thread_notification()) {
        return 1;
    }
    if (test_cancel_race()) {
        return 1;
    }
    if (test_bad_mode()) {
        return 1;
    }
    if (test_ns_aliases()) {
        return 1;
    }
    if (test_multi_request_array()) {
        return 1;
    }
    if (test_signal_notification()) {
        return 1;
    }
    if (test_cancel_all()) {
        return 1;
    }
    if (test_deterministic_error_paths()) {
        return 1;
    }
    if (test_libc_aliases()) {
        return 1;
    }
    return 0;
}
