/**
 * @file nsgai_ns.c
 * @brief Namespace resolution and per-namespace config discovery helpers.
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
 *
 * Network-namespace selection helpers.
 *
 *   - Explicit: caller specifies a name or an fd; we open a netns fd
 *     and the c-ares socket callback transiently setns() into it.
 *
 *   - Implicit: the calling thread's current netns. We do not setns;
 *     we just look at /proc/thread-self/ns/net to see if it matches a
 *     name under /var/run/netns/ so we can pick up that name's
 *     /etc/netns/<name>/{resolv.conf,hosts}.
 */

#include "nsgai_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif   // ndef PATH_MAX

/** @name Path Helpers */
/** @{ */

/**
 * @brief Build a formatted path and return it only when readable.
 */
static char *
build_path_if_exists(const char *fmt, const char *name)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), fmt, name) >= (int)sizeof(path)) {
        return NULL;
    }
    if (access(path, R_OK) != 0) {
        return NULL;
    }
    return strdup(path);
}

/**
 * @brief Populate per-netns resolv.conf and hosts paths for @p name.
 */
static void
fill_paths_for_name(const char *name, nsgai_ns_t *out)
{
    out->resolvconf_path =
        build_path_if_exists("/etc/netns/%s/resolv.conf", name);
    out->hosts_path = build_path_if_exists("/etc/netns/%s/hosts", name);
}

/** @} */

/** @name Name Validation And Open */
/** @{ */

/**
 * @brief Validate namespace name for safe path construction.
 */
static int
validate_ns_name(const char *name)
{
    if (!name || !*name) {
        return -1;
    }
    if (strchr(name, '/')) {
        return -1;
    }
    if (strstr(name, "..")) {
        return -1;
    }
    return 0;
}

/**
 * @brief Open namespace file descriptor from /var/run/netns.
 */
static int
open_netns_by_name(const char *name)
{
    if (validate_ns_name(name) != 0) {
        errno = EINVAL;
        return -1;
    }
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "/var/run/netns/%s", name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return open(path, O_RDONLY | O_CLOEXEC);
}

/** @} */

/**
 * @brief Resolve netns name by inode match under /var/run/netns.
 */

static char *
netns_name_from_inode(dev_t dev, ino_t ino)
{
    DIR *d = opendir("/var/run/netns");
    if (!d) {
        return NULL;
    }

    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "/var/run/netns/%s", ent->d_name) >=
            (int)sizeof(path)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (st.st_dev == dev && st.st_ino == ino) {
            result = strdup(ent->d_name);
            break;
        }
    }
    closedir(d);
    return result;
}

/** @name Public Namespace Resolution API */
/** @{ */

int
nsgai_ns_resolve_explicit_name(const char *ns_name, nsgai_ns_t *out)
{
    memset(out, 0, sizeof(*out));
    out->netns_fd = -1;

    int fd = open_netns_by_name(ns_name);
    if (fd < 0) {
        return -1;
    }
    out->netns_fd      = fd;
    out->owns_netns_fd = true;
    fill_paths_for_name(ns_name, out);
    return 0;
}

int
nsgai_ns_resolve_explicit_fd(int ns_fd, nsgai_ns_t *out)
{
    memset(out, 0, sizeof(*out));
    out->netns_fd = -1;

    if (ns_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    int dup_fd = fcntl(ns_fd, F_DUPFD_CLOEXEC, 3);
    if (dup_fd < 0) {
        return -1;
    }
    out->netns_fd      = dup_fd;
    out->owns_netns_fd = true;

    /* Look up the name (if any) so we can grab per-ns config files. */
    struct stat st;
    if (fstat(dup_fd, &st) == 0) {
        char *name = netns_name_from_inode(st.st_dev, st.st_ino);
        if (name) {
            fill_paths_for_name(name, out);
            free(name);
        }
    }
    return 0;
}

/** @} */

int
nsgai_ns_resolve_implicit(nsgai_ns_t *out)
{
    memset(out, 0, sizeof(*out));
    out->netns_fd = -1; /* no setns - we're already in this ns */

    /* If the thread's current netns matches one named under
     * /var/run/netns/, pick up that name's per-ns config. */
    struct stat st;
    if (stat("/proc/thread-self/ns/net", &st) == 0) {
        char *name = netns_name_from_inode(st.st_dev, st.st_ino);
        if (name) {
            fill_paths_for_name(name, out);
            free(name);
        }
    }
    return 0;
}

void
nsgai_ns_release(nsgai_ns_t *ns)
{
    if (!ns) {
        return;
    }
    if (ns->owns_netns_fd && ns->netns_fd >= 0) {
        close(ns->netns_fd);
    }
    free(ns->resolvconf_path);
    free(ns->hosts_path);
    memset(ns, 0, sizeof(*ns));
    ns->netns_fd = -1;
}
