/**
 * @file nsgai_diag.c
 * @brief Internal diagnostics helpers for pthread error handling.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 */

#include "nsgai_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <syslog.h>

static void
nsgai_log_pthread_failure(int priority, const char *what, int rc)
{
    /* Keep diagnostics process-wide and async-safe enough for fatal paths. */
    syslog(priority, "nsgai: %s failed: rc=%d", what, rc);
}

static void
nsgai_pthread_abort(const char *what, int rc)
{
    nsgai_log_pthread_failure(LOG_CRIT, what, rc);
    /* Synchronization failure means state is no longer trustworthy. */
    abort();
}

void
nsgai_pthread_mutex_lock_or_abort(pthread_mutex_t *mu, const char *what)
{
    int rc = pthread_mutex_lock(mu);
    if (rc != 0) {
        nsgai_pthread_abort(what, rc);
    }
}

void
nsgai_pthread_mutex_unlock_or_abort(pthread_mutex_t *mu, const char *what)
{
    int rc = pthread_mutex_unlock(mu);
    if (rc != 0) {
        nsgai_pthread_abort(what, rc);
    }
}

void
nsgai_pthread_cond_wait_or_abort(
    pthread_cond_t *cv, pthread_mutex_t *mu, const char *what)
{
    int rc = pthread_cond_wait(cv, mu);
    if (rc != 0) {
        nsgai_pthread_abort(what, rc);
    }
}

void
nsgai_pthread_cond_signal_or_abort(pthread_cond_t *cv, const char *what)
{
    int rc = pthread_cond_signal(cv);
    if (rc != 0) {
        nsgai_pthread_abort(what, rc);
    }
}

int
nsgai_pthread_mutex_init_checked(pthread_mutex_t *mu, const char *what)
{
    int rc = pthread_mutex_init(mu, NULL);
    if (rc != 0) {
        nsgai_log_pthread_failure(LOG_ERR, what, rc);
    }
    return rc;
}

int
nsgai_pthread_cond_init_checked(pthread_cond_t *cv, const char *what)
{
    int rc = pthread_cond_init(cv, NULL);
    if (rc != 0) {
        nsgai_log_pthread_failure(LOG_ERR, what, rc);
    }
    return rc;
}

void
nsgai_pthread_mutex_destroy_checked(pthread_mutex_t *mu, const char *what)
{
    int rc = pthread_mutex_destroy(mu);
    if (rc != 0) {
        nsgai_log_pthread_failure(LOG_ERR, what, rc);
    }
}

void
nsgai_pthread_cond_destroy_checked(pthread_cond_t *cv, const char *what)
{
    int rc = pthread_cond_destroy(cv);
    if (rc != 0) {
        nsgai_log_pthread_failure(LOG_ERR, what, rc);
    }
}