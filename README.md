<!--
SPDX-License-Identifier: Apache-2.0

Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# nsgai — network-namespace-aware getaddrinfo via c-ares

`nsgai` is a small Linux library that provides
`getaddrinfo()` / `freeaddrinfo()` / `getnameinfo()` replacements,
plus the GNU `getaddrinfo_a()` / `gai_*()` asynchronous family,
which can resolve names in a specific **network namespace** while the
calling process stays multi-tenant — i.e. without `fork()`, without
moving the caller's thread to a different netns, and without rebuilding
the resolver per request.

The intended use case is processes that simultaneously serve endpoints
in *different* netns.  **rsyslogd** is the original motivating case,
as it supports socket endpoints across multiple network namespaces.
Other examples include sidecars, proxies, and tenant-aware daemons.
For single-ns applications, prefer `ip netns exec NAME …` — which
already bind-mounts
`/etc/netns/NAME/resolv.conf` over `/etc/resolv.conf` — and stay on
libc.

It ships three shared objects:

| Shared object              | Purpose                                                  |
|----------------------------|----------------------------------------------------------|
| `libnsgai.so`              | Public C API: `nsgai_getaddrinfo*`, `nsgai_getnameinfo*`, `nsgai_getaddrinfo_a*`, `nsgai_suspend`, `nsgai_error`, `nsgai_cancel`, `nsgai_freeaddrinfo`, `nsgai_gai_strerror`, plus a process-wide `nsgai_set_lookup_policy()` setter for host-vs-DNS ordering. Also defines the POSIX/GNU-named symbols (`getaddrinfo`, `getaddrinfo_a`, `gai_*`, …) as **weak** aliases — linking against `libnsgai` replaces libc's resolver. |
| `libnsgai_preload.so`      | `LD_PRELOAD` shim. **Unconditionally** forwards `getaddrinfo`/`freeaddrinfo`/`gai_strerror`/`getnameinfo` to `nsgai_*`. There is no fall-through to libc — preloading is opting in for the whole process. |
| `libnss_nsgai.so.2`        | NSS module (`_nss_nsgai_gethostbyname[2/4]_r`, `_nss_nsgai_gethostbyaddr[2]_r`). Usable from `/etc/nsswitch.conf`. |

## API surface

```c
typedef enum {
    NSGAI_LOOKUP_HOSTS_FIRST = 0,
    NSGAI_LOOKUP_DNS_FIRST   = 1,
    NSGAI_LOOKUP_HOSTS_ONLY  = 2,
    NSGAI_LOOKUP_DNS_ONLY    = 3,
} nsgai_lookup_policy_t;

int  nsgai_set_lookup_policy(nsgai_lookup_policy_t policy);

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

#ifdef __USE_GNU
int nsgai_getaddrinfo_a(int mode, struct gaicb *list[], int ent,
                        struct sigevent *sig);
int nsgai_getaddrinfo_a_ns(const char *ns_name, int mode,
                           struct gaicb *list[], int ent,
                           struct sigevent *sig);
int nsgai_getaddrinfo_a_nsfd(int ns_fd, int mode,
                             struct gaicb *list[], int ent,
                             struct sigevent *sig);
int nsgai_suspend(const struct gaicb *const list[], int ent,
                  const struct timespec *timeout);
int nsgai_error(struct gaicb *req);
int nsgai_cancel(struct gaicb *req);
#endif
```

That's it.  There is intentionally no thread-local "default ns"
override and no environment-variable-based selection — they were
removed in v0.2 because they made the chain-of-ownership of returned
buffers unpredictable and obscured *who decides* which ns a lookup
runs in.

The GNU async entry points follow the same namespace-selection rules as
the synchronous API:

* `nsgai_getaddrinfo_a()` resolves in the calling thread's current
  namespace.
* `nsgai_getaddrinfo_a_ns()` targets a named namespace, with `NULL`
  meaning the calling thread's current namespace.
* `nsgai_getaddrinfo_a_nsfd()` targets a namespace fd, with a negative
  fd meaning the calling thread's current namespace.

The synchronous and asynchronous lookup entry points all use a single
process-wide policy. `nsgai_set_lookup_policy()` changes that policy for
subsequent requests. The built-in default is `NSGAI_LOOKUP_HOSTS_FIRST`.
Supported policies are:

* `NSGAI_LOOKUP_HOSTS_FIRST`: consult the namespace hosts file first,
  then DNS.
* `NSGAI_LOOKUP_DNS_FIRST`: consult DNS first, then the namespace hosts
  file as fallback.
* `NSGAI_LOOKUP_HOSTS_ONLY`: only consult the namespace hosts file,
  apart from numeric address/service handling.
* `NSGAI_LOOKUP_DNS_ONLY`: only consult DNS, with the namespace hosts
  file disabled for that lookup.

## How the namespace is chosen

* **Implicit entry points** (`nsgai_getaddrinfo`, `nsgai_getnameinfo`,
  NSS, LD_PRELOAD): the lookup runs in the calling thread's current
  netns.  We `stat("/proc/thread-self/ns/net")` and, if its inode
  matches an entry under `/var/run/netns/`, we pick up that name's
  `/etc/netns/<name>/{resolv.conf,hosts}`.  No `setns()` is performed —
  we're already there.  If the thread is in the root ns (or any ns
  with no name registered), `/etc/resolv.conf` and `/etc/hosts` are
  used as usual.

  > **The namespace must be named under `/var/run/netns/`.**  Per-ns
  > config files are found by matching the thread's netns inode against
  > entries in `/var/run/netns/`.  This directory is only populated when
  > the namespace was created with `ip netns add NAME` (or manually
  > bind-mounted there).  Namespaces created with `unshare --net` or
  > `clone(CLONE_NEWNET)` alone have no entry in `/var/run/netns/` and
  > will not pick up `/etc/netns/` config — they silently fall back to
  > the default resolver files.  Verify with `ip netns identify $$`
  > inside the target namespace.

* **Explicit `_ns(name, …)`**: resolves using `/var/run/netns/<name>`
  and, if present, `/etc/netns/<name>/{resolv.conf,hosts}`.

* **Explicit `_nsfd(fd, …)`**: resolves using the netns referred to by
  `fd` (which must be an `nsfs` file, e.g. opened from
  `/proc/<pid>/ns/net` or `/var/run/netns/<name>`).  If `fd`'s inode
  matches an entry under `/var/run/netns/`, the corresponding per-ns
  config files are picked up automatically.

### Resolver File Fallback Semantics

Resolver-file selection is intentionally **per file** and applies to both
implicit and explicit entry points:

* If `/etc/netns/<name>/resolv.conf` is readable, nsgai uses it;
  otherwise resolution falls back to the default resolver file path
  (`/etc/resolv.conf` in the process mount namespace).
* If `/etc/netns/<name>/hosts` is readable, nsgai uses it; otherwise
  host-file lookups fall back to the default hosts file path
  (`/etc/hosts` in the process mount namespace).
* The two files are independent: one may be overridden while the other
  still uses the default.

This matches common operator expectations for `ip netns exec` and allows
intentional partial per-netns configuration.

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
wrapper from the repository root. It builds one of several supported
distro images (see `docker/README.md` for the full platform matrix:
Fedora, Ubuntu 22.04/24.04, Debian 12, Rocky Linux 9):

```sh
bash scripts/run-container-tests.sh                       # fedora (default)
bash scripts/run-container-tests.sh --platform ubuntu22.04
bash scripts/run-container-tests.sh --all                 # every platform
```

Images and build trees are tagged with the host's architecture
(normalized to `amd64`/`arm64`), so the same platform can be built/tested
on both x86_64 and arm64 machines (or under `qemu-user-static` emulation)
without the two clobbering each other. Override auto-detection with
`--arch`:

```sh
bash scripts/run-container-tests.sh --platform ubuntu22.04 --arch arm64
```

This creates and uses `./build-container-<platform>-<arch>/` as a
container-owned build tree. That directory is configured inside the
container with source path `/src`, so do not reuse it as a normal host
build tree with commands such as `cmake --build build-container-<platform>-<arch>`
on the host. Use the regular `./build/` tree for host builds instead. The
container entrypoint recreates the build tree on each run so image
toolchain changes cannot leave behind stale objects or incompatible
coverage metadata.

If you want to reset a container build tree completely, remove it and
rerun the wrapper:

```sh
podman unshare rm -rf build-container-fedora-arm64
bash scripts/run-container-tests.sh
```

With docker or rootful podman, plain `rm -rf build-container-fedora-arm64`
is usually sufficient.

Each container image carries its own toolchain, including `clang-format`.
If `nsgai_format_check` passes on the host but fails in a container or
vice versa, first rebuild the image so the container toolchain matches
the corresponding `docker/Dockerfile.<platform>` more closely:

```sh
bash scripts/run-container-tests.sh --platform ubuntu22.04 --rebuild
```

Formatting differences across environments are usually caused by running
different `clang-format` versions.

For host-side coverage builds, you can force the reporting backend at
configure time:

```sh
cmake -S . -B build-cov \
    -DNSGAI_ENABLE_COVERAGE=ON \
    -DNSGAI_COVERAGE_TOOL=gcovr
```

Valid values are `auto`, `lcov`, and `gcovr`. `auto` prefers
`lcov`+`genhtml` when available and otherwise falls back to `gcovr`.

## Use as a direct replacement

```sh
cc myapp.c -L/path/to/nsgai/build -lnsgai -Wl,-rpath,/path/to/nsgai/build
```

`getaddrinfo`/`freeaddrinfo`/`getnameinfo` are now `nsgai_*` — calls
are resolved in the calling thread's netns.

If you install the library, a pkg-config file is also provided:

```sh
pkg-config --cflags --libs nsgai
```

## Use as LD_PRELOAD (non-transparent)

```sh
LD_PRELOAD=/path/to/build/libnsgai_preload.so.0 myapp
```

Every name-resolution call from the process now goes through nsgai
with current-thread-netns semantics.  There is no per-call gate and
no fall-through — if you preload `libnsgai_preload.so`, you are
opting `myapp` in to nsgai entirely.

> **Programs that implement their own resolver are not affected.**
> `host`, `dig`, and `nslookup` (from bind-utils / BIND9) call
> libresolv directly and never invoke `getaddrinfo`, so preloading
> has no effect on them.  To test the preload, use a tool that calls
> `getaddrinfo`, for example:
>
> ```sh
> LD_PRELOAD=/usr/lib64/libnsgai_preload.so.0 getent hosts server.test
> ```

> **`libnsgai_preload.so` depends on `libnsgai.so.0` and `libcares.so.2`.**
> Both must be visible to the dynamic linker when the preload is loaded —
> either installed to a system library directory with `ldconfig` run
> afterward, or present in `LD_LIBRARY_PATH`.  If either dependency
> cannot be resolved, the dynamic linker silently skips the preload and
> the process runs with unmodified libc resolution (no error is printed).
> Verify the installed library before testing:
>
> ```sh
> readelf -d /usr/lib64/libnsgai_preload.so.0 | grep NEEDED
> ldconfig -p | grep -E 'nsgai|cares'
> ```

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

> **glibc does not honour `LD_LIBRARY_PATH` for NSS modules.**
> The NSS loader uses a fixed set of system library directories
> (e.g. `/usr/lib64/` or the distribution's multiarch path) and ignores
> `LD_LIBRARY_PATH` for security reasons.  Install `libnss_nsgai.so.2`
> to the appropriate system directory and run `ldconfig` afterward.
> Placing the file elsewhere and setting `LD_LIBRARY_PATH` will not work.

## Multi-tenant integration (rsyslogd)

This is the original motivating case.  rsyslogd allows endpoints in
multiple network namespaces, but its address resolution does not
follow the endpoint's network namespace.  With nsgai, rsyslogd can
resolve those endpoints without forking, moving the caller's thread,
or rebuilding the resolver, while serving arbitrarily many tenants in
one process.

## Limitations / future work

* The service-name database (`/etc/services`) is not namespaced.
* TTL plumbing through `_nss_nsgai_gethostbyname4_r` is minimal.
* ELF symbol visibility/versioning is enforced via `src/nsgai.map`
  (namespace `NSGAI_0`), but ABI compatibility checks are not yet
  automated in CI.

## Contributing

Contributions are welcome. Please read
[CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request.
Issues and proposed changes can be submitted through the
[GitHub repository](https://github.com/cisco-open/nsgai).

## License

This project is licensed under the Apache License, Version 2.0. See
[LICENSE](LICENSE) for the full license text.
