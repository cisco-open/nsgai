/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 */

#include "nsgai.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

static struct {
    int calloc_call_count;
    int calloc_fail_nth;

    int armed_pthread_create;
    int pthread_create_rc;

    int armed_clock_gettime;

    int armed_cond_timedwait;
    int cond_timedwait_rc;
} g_wrap;

extern void *
__real_calloc(size_t, size_t);
void *
__wrap_calloc(size_t nmemb, size_t size)
{
    g_wrap.calloc_call_count++;
    if (g_wrap.calloc_fail_nth > 0 &&
        g_wrap.calloc_call_count == g_wrap.calloc_fail_nth) {
        return NULL;
    }
    return __real_calloc(nmemb, size);
}

extern int
__real_pthread_create(
    pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int
__wrap_pthread_create(
    pthread_t *thr, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    if (g_wrap.armed_pthread_create) {
        g_wrap.armed_pthread_create = 0;
        return g_wrap.pthread_create_rc;
    }
    return __real_pthread_create(thr, attr, fn, arg);
}

extern int
__real_clock_gettime(clockid_t, struct timespec *);
int
__wrap_clock_gettime(clockid_t clk, struct timespec *tp)
{
    if (g_wrap.armed_clock_gettime) {
        g_wrap.armed_clock_gettime = 0;
        errno                      = EINVAL;
        return -1;
    }
    return __real_clock_gettime(clk, tp);
}

extern int
__real_pthread_cond_timedwait(
    pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int
__wrap_pthread_cond_timedwait(
    pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *abs)
{
    if (g_wrap.armed_cond_timedwait) {
        g_wrap.armed_cond_timedwait = 0;
        return g_wrap.cond_timedwait_rc;
    }
    return __real_pthread_cond_timedwait(cv, mu, abs);
}

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

    timeout.tv_sec  = 5;
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
test_nowait_allocation_and_worker_failures(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct addrinfo hints;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    memset(&g_wrap, 0, sizeof(g_wrap));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;
    list[0]         = &req;

    /* to_submit allocation failure in NOWAIT path. */
    g_wrap.calloc_fail_nth = 1;
    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == EAI_MEMORY);

    memset(&g_wrap, 0, sizeof(g_wrap));
    g_wrap.armed_pthread_create = 1;
    g_wrap.pthread_create_rc    = EAGAIN;
    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == EAI_AGAIN);

    memset(&g_wrap, 0, sizeof(g_wrap));
    g_wrap.calloc_fail_nth = 2;
    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == EAI_MEMORY);

    return 0;
}

static int
test_wait_with_null_entries(void)
{
    struct gaicb req;
    struct gaicb *list[2];
    struct addrinfo hints;

    memset(&req, 0, sizeof(req));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags  = AI_NUMERICHOST;
    req.ar_name     = "127.0.0.1";
    req.ar_request  = &hints;

    list[0] = NULL;
    list[1] = &req;

    MUST(nsgai_getaddrinfo_a(GAI_WAIT, list, 2, NULL) == 0);
    MUST(req.__return == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    return 0;
}

static int
test_nowait_ns_and_null_hints_paths(void)
{
    struct gaicb req1, req2, req3;
    struct gaicb *list[1];

    memset(&req1, 0, sizeof(req1));
    memset(&req2, 0, sizeof(req2));
    memset(&req3, 0, sizeof(req3));

    req1.ar_name    = "127.0.0.1";
    req1.ar_request = NULL;
    list[0]         = &req1;
    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req1) == 0);
    MUST(nsgai_error(&req1) == 0);
    MUST(req1.ar_result != NULL);
    nsgai_freeaddrinfo(req1.ar_result);

    req2.ar_name = "127.0.0.1";
    list[0]      = &req2;
    MUST(nsgai_getaddrinfo_a_ns(NULL, GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req2) == 0);
    MUST(nsgai_error(&req2) == 0);
    MUST(req2.ar_result != NULL);
    nsgai_freeaddrinfo(req2.ar_result);

    req3.ar_name = "127.0.0.1";
    list[0]      = &req3;
    MUST(nsgai_getaddrinfo_a_nsfd(-1, GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req3) == 0);
    MUST(nsgai_error(&req3) == 0);
    MUST(req3.ar_result != NULL);
    nsgai_freeaddrinfo(req3.ar_result);

    return 0;
}

static int
test_nowait_nsfd_explicit_path(void)
{
    int nsfd;
    struct gaicb req;
    struct gaicb *list[1];

    nsfd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    MUST(nsfd >= 0);

    memset(&req, 0, sizeof(req));
    req.ar_name = "127.0.0.1";
    list[0]     = &req;

    MUST(nsgai_getaddrinfo_a_nsfd(nsfd, GAI_NOWAIT, list, 1, NULL) == 0);
    MUST(wait_for_completion((const struct gaicb *const *)list, 1, &req) == 0);
    MUST(nsgai_error(&req) == 0);
    MUST(req.ar_result != NULL);
    nsgai_freeaddrinfo(req.ar_result);
    close(nsfd);
    return 0;
}

static int
test_suspend_clock_and_timedwait_errors(void)
{
    struct gaicb req;
    const struct gaicb *list[1];
    struct timespec timeout;

    memset(&req, 0, sizeof(req));
    memset(&g_wrap, 0, sizeof(g_wrap));
    req.__return    = EAI_INPROGRESS;
    list[0]         = &req;
    timeout.tv_sec  = 0;
    timeout.tv_nsec = 1000000;

    g_wrap.armed_clock_gettime = 1;
    MUST(nsgai_suspend(list, 1, &timeout) == EAI_SYSTEM);

    g_wrap.armed_cond_timedwait = 1;
    g_wrap.cond_timedwait_rc    = EINVAL;
    MUST(nsgai_suspend(list, 1, &timeout) == EAI_SYSTEM);

    return 0;
}

static int
test_invalid_inputs_and_suspend_edges(void)
{
    struct gaicb req;
    struct gaicb *list[1];
    struct timespec timeout;

    memset(&req, 0, sizeof(req));
    list[0] = &req;

    MUST(nsgai_getaddrinfo_a(GAI_NOWAIT, list, -1, NULL) == EAI_SYSTEM);
    MUST(nsgai_getaddrinfo_a_ns(
             "definitely-no-such-ns", GAI_NOWAIT, list, 1, NULL) == EAI_SYSTEM);
    MUST(nsgai_getaddrinfo_a_nsfd(999999, GAI_NOWAIT, list, 1, NULL) ==
         EAI_SYSTEM);

    MUST(nsgai_suspend((const struct gaicb *const *)list, -1, NULL) ==
         EAI_SYSTEM);
    MUST(nsgai_suspend((const struct gaicb *const *)list, 0, NULL) ==
         EAI_ALLDONE);

    timeout.tv_sec  = 0;
    timeout.tv_nsec = 1000000;
    MUST(nsgai_suspend((const struct gaicb *const *)list, 0, &timeout) ==
         EAI_ALLDONE);

    MUST(nsgai_error(NULL) == EAI_SYSTEM);

    req.__return = EAI_INPROGRESS;
    {
        int cancel_rc = nsgai_cancel(&req);
        MUST(cancel_rc == EAI_NOTCANCELED || cancel_rc == EAI_ALLDONE);
    }
    req.__return = 0;
    MUST(nsgai_cancel(&req) == EAI_ALLDONE);

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
    MUST(cancel_rc == EAI_CANCELED || cancel_rc == EAI_ALLDONE);

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

    MUST(nsgai_cancel(NULL) == EAI_ALLDONE);

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
    if (test_nowait_allocation_and_worker_failures()) {
        return 1;
    }
    if (test_wait_with_null_entries()) {
        return 1;
    }
    if (test_nowait_ns_and_null_hints_paths()) {
        return 1;
    }
    if (test_nowait_nsfd_explicit_path()) {
        return 1;
    }
    if (test_suspend_clock_and_timedwait_errors()) {
        return 1;
    }
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
    if (test_invalid_inputs_and_suspend_edges()) {
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
