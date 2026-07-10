/**
 * @file nsgai_async.c
 * @brief GNU-style asynchronous getaddrinfo implementation.
 *
 * SPDX-License-Identifier: Apache-2.0
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
 */

#include "nsgai_internal.h"

#ifdef __USE_GNU

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct nsgai_async_req_s {
    struct gaicb *req;
    nsgai_ns_t ns;
    nsgai_channel_t *channel;
    int done;
    int notified;
    struct sigevent sig;
    int have_sig;
    struct nsgai_async_req_s *next;
} nsgai_async_req_t;

static pthread_mutex_t g_async_mu      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_async_cv       = PTHREAD_COND_INITIALIZER;
static nsgai_async_req_t *g_async_head = NULL;
static pthread_t g_async_worker;
static int g_async_worker_started = 0;

static void
nsgai_async_broadcast_or_abort(void)
{
    int rc = pthread_cond_broadcast(&g_async_cv);
    if (rc != 0) {
        abort();
    }
}

static int
nsgai_timespec_abs_from_rel(
    const struct timespec *rel, struct timespec *abs_out)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }

    abs_out->tv_sec  = now.tv_sec + rel->tv_sec;
    abs_out->tv_nsec = now.tv_nsec + rel->tv_nsec;
    if (abs_out->tv_nsec >= 1000000000L) {
        abs_out->tv_sec++;
        abs_out->tv_nsec -= 1000000000L;
    }
    return 0;
}

typedef struct {
    void (*fn)(union sigval);
    union sigval value;
    pthread_attr_t *attrs;
} nsgai_async_thread_notify_t;

static void *
nsgai_async_thread_notify_start(void *arg)
{
    nsgai_async_thread_notify_t *notify = (nsgai_async_thread_notify_t *)arg;

    notify->fn(notify->value);
    free(notify);
    return NULL;
}

static void
nsgai_async_notify(const struct sigevent *sig)
{
    if (sig == NULL) {
        return;
    }

    switch (sig->sigev_notify) {
        case SIGEV_NONE: break;
        case SIGEV_SIGNAL:
            (void)sigqueue(getpid(), sig->sigev_signo, sig->sigev_value);
            break;
        case SIGEV_THREAD: {
            pthread_t tid;
            nsgai_async_thread_notify_t *notify =
                (nsgai_async_thread_notify_t *)calloc(1, sizeof(*notify));
            if (notify == NULL) {
                break;
            }
            notify->fn    = sig->sigev_notify_function;
            notify->value = sig->sigev_value;
            notify->attrs = sig->sigev_notify_attributes;
            if (pthread_create(&tid, notify->attrs,
                    nsgai_async_thread_notify_start, notify) == 0) {
                pthread_detach(tid);
            } else {
                free(notify);
            }
            break;
        }
        default: break;
    }
}

static nsgai_async_req_t *
nsgai_async_find_locked(const struct gaicb *req)
{
    nsgai_async_req_t *cur = g_async_head;

    while (cur != NULL) {
        if (cur->req == req) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static void
nsgai_async_remove_locked(nsgai_async_req_t *victim)
{
    nsgai_async_req_t **link = &g_async_head;

    while (*link != NULL) {
        if (*link == victim) {
            *link        = victim->next;
            victim->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static void
nsgai_async_request_free(nsgai_async_req_t *req)
{
    if (req == NULL) {
        return;
    }
    if (req->channel != NULL) {
        nsgai_channel_destroy(req->channel);
    }
    nsgai_ns_release(&req->ns);
    free(req);
}

static void
nsgai_async_complete_locked(
    nsgai_async_req_t *req, int status, struct addrinfo *result)
{
    if (req->done) {
        if (result != NULL) {
            nsgai_freeaddrinfo(result);
        }
        return;
    }

    if (req->req->__return != EAI_CANCELED) {
        req->req->ar_result = result;
        req->req->__return  = status;
    } else if (result != NULL) {
        nsgai_freeaddrinfo(result);
    }
    req->done = 1;
    nsgai_async_broadcast_or_abort();
}

static void
nsgai_async_ai_callback(
    void *arg, int status, int timeouts, struct ares_addrinfo *result)
{
    nsgai_async_req_t *req     = (nsgai_async_req_t *)arg;
    struct addrinfo *converted = NULL;
    int eai;
    int should_notify = 0;
    struct sigevent sig_copy;

    (void)timeouts;

    if (status != ARES_SUCCESS) {
        eai = nsgai_status_to_eai((ares_status_t)status);
    } else {
        eai = nsgai_convert_addrinfo(
            result, req->req->ar_request, req->req->ar_service, &converted);
    }
    if (result != NULL) {
        ares_freeaddrinfo(result);
    }

    nsgai_pthread_mutex_lock_or_abort(&g_async_mu, "pthread_mutex_lock(async)");
    nsgai_async_complete_locked(req, eai, converted);
    if (!req->notified && req->have_sig) {
        req->notified = 1;
        sig_copy      = req->sig;
        should_notify = 1;
    }
    nsgai_pthread_mutex_unlock_or_abort(
        &g_async_mu, "pthread_mutex_unlock(async)");

    if (should_notify) {
        nsgai_async_notify(&sig_copy);
    }
}

static int
nsgai_async_build_request_from_ns(nsgai_async_req_t **out,
    const nsgai_ns_t *ns,
    struct gaicb *req,
    const struct sigevent *sig)
{
    nsgai_async_req_t *state = (nsgai_async_req_t *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return EAI_MEMORY;
    }

    state->req         = req;
    state->ns.netns_fd = -1;

    if (ns->netns_fd >= 0) {
        if (nsgai_ns_resolve_explicit_fd(ns->netns_fd, &state->ns) != 0) {
            free(state);
            return EAI_SYSTEM;
        }
    } else if (ns->resolvconf_path != NULL || ns->hosts_path != NULL) {
        if (nsgai_ns_resolve_implicit(&state->ns) != 0) {
            free(state);
            return EAI_SYSTEM;
        }
    } else {
        state->ns.netns_fd = -1;
    }

    if (sig != NULL) {
        state->sig      = *sig;
        state->have_sig = 1;
    }

    if (nsgai_channel_create_uncached(&state->ns, &state->channel) !=
        ARES_SUCCESS) {
        nsgai_async_request_free(state);
        return EAI_SYSTEM;
    }

    *out = state;
    return 0;
}

static int
nsgai_async_prepare_request(nsgai_async_req_t **out,
    const char *ns_name,
    int ns_fd,
    int ns_mode,
    struct gaicb *req,
    const struct sigevent *sig)
{
    nsgai_ns_t ns;
    int rc;

    memset(&ns, 0, sizeof(ns));
    ns.netns_fd = -1;

    if (ns_mode == 0) {
        rc = nsgai_ns_resolve_implicit(&ns);
    } else if (ns_mode == 1) {
        rc = (ns_name == NULL) ? nsgai_ns_resolve_implicit(&ns)
                               : nsgai_ns_resolve_explicit_name(ns_name, &ns);
    } else {
        rc = (ns_fd < 0) ? nsgai_ns_resolve_implicit(&ns)
                         : nsgai_ns_resolve_explicit_fd(ns_fd, &ns);
    }
    if (rc != 0) {
        return EAI_SYSTEM;
    }

    rc = nsgai_async_build_request_from_ns(out, &ns, req, sig);
    nsgai_ns_release(&ns);
    return rc;
}

static void
nsgai_async_submit(nsgai_async_req_t *req)
{
    struct ares_addrinfo_hints ah;

    memset(&ah, 0, sizeof(ah));
    if (req->req->ar_request != NULL) {
        ah.ai_flags    = nsgai_map_ai_flags(req->req->ar_request->ai_flags);
        ah.ai_family   = req->req->ar_request->ai_family;
        ah.ai_socktype = req->req->ar_request->ai_socktype;
        ah.ai_protocol = req->req->ar_request->ai_protocol;
    } else {
        ah.ai_family = AF_UNSPEC;
    }

    req->req->ar_result = NULL;
    req->req->__return  = EAI_INPROGRESS;
    ares_getaddrinfo(nsgai_channel_ares(req->channel), req->req->ar_name,
        req->req->ar_service, &ah, nsgai_async_ai_callback, req);
}

static void *
nsgai_async_worker_main(void *arg)
{
    (void)arg;

    for (;;) {
        struct pollfd pfds[NSGAI_MAX_CHANNEL_SOCKETS * 32];
        ares_channel_t *channels[32];
        nsgai_channel_t *channel_handles[32];
        nfds_t channel_pfd_start[32];
        nfds_t channel_pfd_count[32];
        nfds_t channel_count = 0;
        nfds_t nfds          = 0;
        /* Recheck periodically because a request can be submitted after the
         * worker snapshots the channel list but before it enters poll(). */
        int timeout_ms = 100;

        nsgai_pthread_mutex_lock_or_abort(
            &g_async_mu, "pthread_mutex_lock(async)");
        while (g_async_head == NULL) {
            nsgai_pthread_cond_wait_or_abort(
                &g_async_cv, &g_async_mu, "pthread_cond_wait(async)");
        }

        for (nsgai_async_req_t *req = g_async_head; req != NULL;
            req                     = req->next) {
            if (req->done) {
                continue;
            }
            if (channel_count < (sizeof(channels) / sizeof(channels[0]))) {
                channels[channel_count++] = nsgai_channel_ares(req->channel);
                channel_handles[channel_count - 1] = req->channel;
            }
        }
        nsgai_pthread_mutex_unlock_or_abort(
            &g_async_mu, "pthread_mutex_unlock(async)");

        if (channel_count > 0) {
            for (nfds_t c = 0; c < channel_count; ++c) {
                channel_pfd_start[c] = nfds;
                channel_pfd_count[c] = nsgai_channel_pollfds(channel_handles[c],
                    pfds + nfds, (sizeof(pfds) / sizeof(pfds[0])) - nfds);
                nfds += channel_pfd_count[c];

                struct timeval tv;
                struct timeval *tvp = ares_timeout(channels[c], NULL, &tv);

                if (tvp != NULL) {
                    int ms = (int)((long long)tvp->tv_sec * 1000 +
                                   ((long long)tvp->tv_usec + 999) / 1000);
                    if (timeout_ms < 0 || ms < timeout_ms) {
                        timeout_ms = ms;
                    }
                }
            }

            if (nfds == 0 && timeout_ms < 0) {
                for (nfds_t c = 0; c < channel_count; ++c) {
                    nsgai_channel_process_pollfds(channel_handles[c], NULL, 0);
                }
            } else {
                int poll_rc = poll(nfds ? pfds : NULL, nfds, timeout_ms);
                if (poll_rc < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    for (nfds_t c = 0; c < channel_count; ++c) {
                        nsgai_channel_process_pollfds(
                            channel_handles[c], NULL, 0);
                    }
                } else if (poll_rc == 0) {
                    for (nfds_t c = 0; c < channel_count; ++c) {
                        nsgai_channel_process_pollfds(
                            channel_handles[c], NULL, 0);
                    }
                } else {
                    for (nfds_t c = 0; c < channel_count; ++c) {
                        nsgai_channel_process_pollfds(channel_handles[c],
                            pfds + channel_pfd_start[c], channel_pfd_count[c]);
                    }
                }
            }
        }

        nsgai_pthread_mutex_lock_or_abort(
            &g_async_mu, "pthread_mutex_lock(async)");
        nsgai_async_req_t *cur = g_async_head;
        while (cur != NULL) {
            nsgai_async_req_t *next = cur->next;
            if (cur->done) {
                nsgai_async_remove_locked(cur);
                nsgai_pthread_mutex_unlock_or_abort(
                    &g_async_mu, "pthread_mutex_unlock(async)");
                nsgai_async_request_free(cur);
                nsgai_pthread_mutex_lock_or_abort(
                    &g_async_mu, "pthread_mutex_lock(async)");
            }
            cur = next;
        }
        nsgai_pthread_mutex_unlock_or_abort(
            &g_async_mu, "pthread_mutex_unlock(async)");
    }

    return NULL;
}

static int
nsgai_async_ensure_worker(void)
{
    if (g_async_worker_started) {
        return 0;
    }
    if (pthread_create(&g_async_worker, NULL, nsgai_async_worker_main, NULL) !=
        0) {
        return -1;
    }
    pthread_detach(g_async_worker);
    g_async_worker_started = 1;
    return 0;
}

static int
nsgai_getaddrinfo_a_common(const char *ns_name,
    int ns_fd,
    int ns_mode,
    int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig)
{
    int rc                        = 0;
    nsgai_async_req_t **to_submit = NULL;

    if (mode != GAI_WAIT && mode != GAI_NOWAIT) {
        return EAI_SYSTEM;
    }
    if (ent < 0) {
        return EAI_SYSTEM;
    }

    if (mode == GAI_WAIT) {
        for (int i = 0; i < ent; ++i) {
            struct gaicb *req = list[i];
            if (req == NULL) {
                continue;
            }
            if (ns_mode == 0) {
                req->__return = nsgai_getaddrinfo(req->ar_name, req->ar_service,
                    req->ar_request, &req->ar_result);
            } else if (ns_mode == 1) {
                req->__return = nsgai_getaddrinfo_ns(ns_name, req->ar_name,
                    req->ar_service, req->ar_request, &req->ar_result);
            } else {
                req->__return = nsgai_getaddrinfo_nsfd(ns_fd, req->ar_name,
                    req->ar_service, req->ar_request, &req->ar_result);
            }
        }
        return 0;
    }

    if (ent > 0) {
        to_submit =
            (nsgai_async_req_t **)calloc((size_t)ent, sizeof(*to_submit));
        if (to_submit == NULL) {
            return EAI_MEMORY;
        }
    }

    nsgai_pthread_mutex_lock_or_abort(&g_async_mu, "pthread_mutex_lock(async)");
    if (nsgai_async_ensure_worker() != 0) {
        nsgai_pthread_mutex_unlock_or_abort(
            &g_async_mu, "pthread_mutex_unlock(async)");
        free(to_submit);
        return EAI_AGAIN;
    }

    for (int i = 0; i < ent; ++i) {
        nsgai_async_req_t *state = NULL;
        struct gaicb *req        = list[i];
        int req_rc;

        if (req == NULL) {
            continue;
        }
        req_rc = nsgai_async_prepare_request(
            &state, ns_name, ns_fd, ns_mode, req, sig);
        if (req_rc != 0) {
            req->__return = req_rc;
            if (rc == 0) {
                rc = req_rc;
            }
            continue;
        }

        req->ar_result = NULL;
        req->__return  = EAI_INPROGRESS;
        state->next    = g_async_head;
        g_async_head   = state;
        to_submit[i]   = state;
    }
    nsgai_async_broadcast_or_abort();
    nsgai_pthread_mutex_unlock_or_abort(
        &g_async_mu, "pthread_mutex_unlock(async)");

    for (int i = 0; i < ent; ++i) {
        if (to_submit[i] != NULL) {
            nsgai_async_submit(to_submit[i]);
        }
    }
    free(to_submit);

    return rc;
}

int
nsgai_getaddrinfo_a(
    int mode, struct gaicb *list[__restrict_arr], int ent, struct sigevent *sig)
{
    return nsgai_getaddrinfo_a_common(NULL, -1, 0, mode, list, ent, sig);
}

int
nsgai_getaddrinfo_a_ns(const char *ns_name,
    int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig)
{
    return nsgai_getaddrinfo_a_common(ns_name, -1, 1, mode, list, ent, sig);
}

int
nsgai_getaddrinfo_a_nsfd(int ns_fd,
    int mode,
    struct gaicb *list[__restrict_arr],
    int ent,
    struct sigevent *sig)
{
    return nsgai_getaddrinfo_a_common(NULL, ns_fd, 2, mode, list, ent, sig);
}

int
nsgai_suspend(
    const struct gaicb *const list[], int ent, const struct timespec *timeout)
{
    int actual = 0;
    struct timespec abs_timeout;
    const struct timespec *abs_timeout_p = NULL;

    if (ent < 0) {
        return EAI_SYSTEM;
    }

    if (timeout != NULL) {
        if (nsgai_timespec_abs_from_rel(timeout, &abs_timeout) != 0) {
            return EAI_SYSTEM;
        }
        abs_timeout_p = &abs_timeout;
    }

    nsgai_pthread_mutex_lock_or_abort(&g_async_mu, "pthread_mutex_lock(async)");
    for (;;) {
        actual = 0;
        for (int i = 0; i < ent; ++i) {
            const struct gaicb *req = list[i];
            if (req == NULL) {
                continue;
            }
            ++actual;
            if (req->__return != EAI_INPROGRESS) {
                nsgai_pthread_mutex_unlock_or_abort(
                    &g_async_mu, "pthread_mutex_unlock(async)");
                return 0;
            }
        }

        if (actual == 0) {
            nsgai_pthread_mutex_unlock_or_abort(
                &g_async_mu, "pthread_mutex_unlock(async)");
            return EAI_ALLDONE;
        }

        if (timeout == NULL) {
            nsgai_pthread_cond_wait_or_abort(
                &g_async_cv, &g_async_mu, "pthread_cond_wait(async)");
            continue;
        }

        int wait_rc =
            pthread_cond_timedwait(&g_async_cv, &g_async_mu, abs_timeout_p);
        if (wait_rc == ETIMEDOUT) {
            nsgai_pthread_mutex_unlock_or_abort(
                &g_async_mu, "pthread_mutex_unlock(async)");
            return EAI_AGAIN;
        }
        if (wait_rc != 0) {
            nsgai_pthread_mutex_unlock_or_abort(
                &g_async_mu, "pthread_mutex_unlock(async)");
            return EAI_SYSTEM;
        }
    }
}

int
nsgai_error(struct gaicb *req)
{
    if (req == NULL) {
        return EAI_SYSTEM;
    }
    return req->__return;
}

int
nsgai_cancel(struct gaicb *req)
{
    int saw_canceled = 0;
    int saw_any      = 0;
    struct sigevent sig_copy;
    int do_notify = 0;

    nsgai_pthread_mutex_lock_or_abort(&g_async_mu, "pthread_mutex_lock(async)");
    if (req != NULL) {
        nsgai_async_req_t *state = nsgai_async_find_locked(req);
        if (state == NULL) {
            int ret = (req->__return == EAI_INPROGRESS) ? EAI_NOTCANCELED
                                                        : EAI_ALLDONE;
            nsgai_pthread_mutex_unlock_or_abort(
                &g_async_mu, "pthread_mutex_unlock(async)");
            return ret;
        }
        if (!state->done) {
            req->__return = EAI_CANCELED;
            ares_cancel(nsgai_channel_ares(state->channel));
            saw_canceled = 1;
            if (!state->notified && state->have_sig) {
                state->notified = 1;
                sig_copy        = state->sig;
                do_notify       = 1;
            }
        }
        nsgai_async_broadcast_or_abort();
        nsgai_pthread_mutex_unlock_or_abort(
            &g_async_mu, "pthread_mutex_unlock(async)");
        if (do_notify) {
            nsgai_async_notify(&sig_copy);
        }
        return saw_canceled ? EAI_CANCELED : EAI_ALLDONE;
    }

    for (nsgai_async_req_t *state = g_async_head; state != NULL;
        state                     = state->next) {
        saw_any = 1;
        if (!state->done) {
            state->req->__return = EAI_CANCELED;
            ares_cancel(nsgai_channel_ares(state->channel));
            saw_canceled = 1;
        }
    }
    nsgai_async_broadcast_or_abort();
    nsgai_pthread_mutex_unlock_or_abort(
        &g_async_mu, "pthread_mutex_unlock(async)");

    if (saw_canceled) {
        return EAI_CANCELED;
    }
    if (!saw_any) {
        return EAI_ALLDONE;
    }
    return EAI_ALLDONE;
}

__attribute__((weak, visibility("default"))) int
getaddrinfo_a(
    int mode, struct gaicb *list[__restrict_arr], int ent, struct sigevent *sig)
{
    return nsgai_getaddrinfo_a(mode, list, ent, sig);
}

__attribute__((weak, visibility("default"))) int
gai_suspend(
    const struct gaicb *const list[], int ent, const struct timespec *timeout)
{
    return nsgai_suspend(list, ent, timeout);
}

__attribute__((weak, visibility("default"))) int
gai_error(struct gaicb *req)
{
    return nsgai_error(req);
}

__attribute__((weak, visibility("default"))) int
gai_cancel(struct gaicb *req)
{
    return nsgai_cancel(req);
}

#endif
