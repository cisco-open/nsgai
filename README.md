<!--
SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
All rights reserved.
-->

# nsgai — network-namespace-aware getaddrinfo via c-ares

`nsgai` is a small Linux library that provides
`getaddrinfo()` / `freeaddrinfo()` / `getnameinfo()` replacements
which can resolve names in a specific **network namespace** while the
calling process stays multi-tenant — i.e. without `fork()`, without
moving the caller's thread to a different netns, and without rebuilding
the resolver per request.

The intended use case is processes that simultaneously serve endpoints
in *different* netns (e.g. **rsyslogd** with per-input network
namespaces, sidecars/proxies, tenant-aware daemons).  For single-ns
applications, prefer `ip netns exec NAME …` — which already bind-mounts
`/etc/netns/NAME/resolv.conf` over `/etc/resolv.conf` — and stay on
libc.

It ships three shared objects:

| Shared object              | Purpose                                                  |
|----------------------------|----------------------------------------------------------|
| `libnsgai.so`              | Public C API: `nsgai_getaddrinfo`, `nsgai_getaddrinfo_ns`, `nsgai_getaddrinfo_nsfd`, `nsgai_getnameinfo*`, `nsgai_freeaddrinfo`, `nsgai_gai_strerror`. Also defines the POSIX-named symbols (`getaddrinfo`, …) as **weak** aliases — linking against `libnsgai` replaces libc's resolver. |
| `libnsgai_preload.so`      | `LD_PRELOAD` shim. **Unconditionally** forwards `getaddrinfo`/`freeaddrinfo`/`gai_strerror`/`getnameinfo` to `nsgai_*`. There is no fall-through to libc — preloading is opting in for the whole process. |
| `libnss_nsgai.so.2`        | NSS module (`_nss_nsgai_gethostbyname[2/4]_r`, `_nss_nsgai_gethostbyaddr[2]_r`). Usable from `/etc/nsswitch.conf`. |

## API surface

```c
/* Implicit: resolve in the *calling thread's* current network namespace. */
int  nsgai_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res);
int  nsgai_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                       char *host, socklen_t hostlen,
                       char *serv, socklen_t servlen, int flags);

/* Explicit: target a *different* netns just for this lookup. */
int  nsgai_getaddrinfo_ns  (const char *ns_name, /* + same args */);
int  nsgai_getaddrinfo_nsfd(int ns_fd,           /* + same args */);
int  nsgai_getnameinfo_ns  (const char *ns_name, /* + same args */);
int  nsgai_getnameinfo_nsfd(int ns_fd,           /* + same args */);

void nsgai_freeaddrinfo(struct addrinfo *res);
const char *nsgai_gai_strerror(int errcode);
```

That's it.  There is intentionally no thread-local "default ns"
override and no environment-variable-based selection — they were
removed in v0.2 because they made the chain-of-ownership of returned
buffers unpredictable and obscured *who decides* which ns a lookup
runs in.

## How the namespace is chosen

* **Implicit entry points** (`nsgai_getaddrinfo`, `nsgai_getnameinfo`,
  NSS, LD_PRELOAD): the lookup runs in the calling thread's current
  netns.  We `stat("/proc/thread-self/ns/net")` and, if its inode
  matches an entry under `/var/run/netns/`, we pick up that name's
  `/etc/netns/<name>/{resolv.conf,hosts}`.  No `setns()` is performed —
  we're already there.  If the thread is in the root ns (or any ns
  with no name registered), `/etc/resolv.conf` and `/etc/hosts` are
  used as usual.

* **Explicit `_ns(name, …)`**: resolves using `/var/run/netns/<name>`
  and, if present, `/etc/netns/<name>/{resolv.conf,hosts}`.

* **Explicit `_nsfd(fd, …)`**: resolves using the netns referred to by
  `fd` (which must be an `nsfs` file, e.g. opened from
  `/proc/<pid>/ns/net` or `/var/run/netns/<name>`).  If `fd`'s inode
  matches an entry under `/var/run/netns/`, the corresponding per-ns
  config files are picked up automatically.

## How no-fork namespace entry works

For the explicit entry points, c-ares is given a custom set of socket
functions via `ares_set_socket_functions_ex()`.  Only `asocket()` does
any work: it saves c-ares' own event-thread netns
(`/proc/thread-self/ns/net`), calls `setns(target, CLONE_NEWNET)`,
creates the socket, then restores the saved netns.  The socket retains
its binding to the target netns for its lifetime, and because the
setns happens on the c-ares event thread, the caller's thread is
never disturbed.

The implicit path skips the socket-callback dance entirely: c-ares
just creates sockets in whatever netns the caller's thread is already
in.

## Build

Requires CMake ≥ 3.16, a C11 compiler, and c-ares
(≥ the version that exposes `ares_set_socket_functions_ex` and
`ARES_OPT_EVENT_THREAD`; recent main is fine).

```sh
cd /home/balsup/nsgai/main
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/c-ares/install/usr
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Privileged integration tests (optional)

The DNS-aware integration tests use real network namespaces and a
CoreDNS instance per namespace, so they need root and a `coredns`
binary.  Enable them at configure time and run via `sudo ctest`:

```sh
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH=/path/to/c-ares/install/usr \
    -DNSGAI_ENABLE_NETNS_TESTS=ON
cmake --build build -j

# Either let the ctest fixture fetch CoreDNS automatically …
sudo env LD_LIBRARY_PATH=/path/to/c-ares/install/usr/lib64:$PWD/build \
    ctest --test-dir build --output-on-failure -R netns

# … or pre-fetch it explicitly (e.g. on an air-gapped runner):
cmake --build build --target fetch-coredns
```

Tests that need missing prerequisites (no root, no `ip`, no `coredns`)
exit with status 77 and are reported as **Skipped** rather than failed.

### Container test runner

For the full privileged test suite plus coverage report, use the project
wrapper from the repository root:

```sh
cd /home/balsup/nsgai/main
bash scripts/run-container-tests.sh
```

This creates and uses `./build-container/` as a container-owned build
tree. That directory is configured inside the container with source path
`/src`, so do not reuse it as a normal host build tree with commands
such as `cmake --build build-container` on the host. Use the regular
`./build/` tree for host builds instead.

If you want to reset the container build tree completely, remove it and
rerun the wrapper:

```sh
cd /home/balsup/nsgai/main
podman unshare rm -rf build-container
bash scripts/run-container-tests.sh
```

With docker or rootful podman, plain `rm -rf build-container` is usually
sufficient.

## Use as a direct replacement

```sh
cc myapp.c -L/path/to/nsgai/build -lnsgai -Wl,-rpath,/path/to/nsgai/build
```

`getaddrinfo`/`freeaddrinfo`/`getnameinfo` are now `nsgai_*` — calls
are resolved in the calling thread's netns.

## Use as LD_PRELOAD (non-transparent)

```sh
LD_PRELOAD=/path/to/build/libnsgai_preload.so myapp
```

Every name-resolution call from the process now goes through nsgai
with current-thread-netns semantics.  There is no per-call gate and
no fall-through — if you preload `libnsgai_preload.so`, you are
opting `myapp` in to nsgai entirely.

## Use as NSS module

Install `libnss_nsgai.so.2` to a directory on the dynamic loader's
path and add `nsgai` to the hosts line:

```
# /etc/nsswitch.conf
hosts: files nsgai dns
```

The module resolves in the calling thread's netns, so it composes
naturally with `ip netns exec` and with multi-tenant daemons that
have already `setns`-ed their handler thread before calling.

## Multi-tenant integration (rsyslogd)

This is the original motivating case.  rsyslogd allows endpoints in
arbitrary network namespaces, but its address resolution does not
follow the per-endpoint netns.  With nsgai, the rsyslogd input/output
plugin opens an `nsfs` fd for the target netns once and calls
`nsgai_getaddrinfo_nsfd(fd, …)` per resolution — no fork, no caller
thread migration, no resolver rebuild, and arbitrarily many tenants
in one process.

## Limitations / future work

* The service-name database (`/etc/services`) is not namespaced.
* TTL plumbing through `_nss_nsgai_gethostbyname4_r` is minimal.
* Reverse `getnameinfo` falls back to c-ares only — no per-namespace
  `/etc/hosts` reverse search beyond what c-ares does via the
  configured `hosts_path`.
* ELF symbol visibility/versioning is enforced via `src/nsgai.map`
  (namespace `NSGAI_0`), but ABI compatibility checks are not yet
  automated in CI.
