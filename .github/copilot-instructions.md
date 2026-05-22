# nsgai — Agent Instructions

nsgai is a C11 shared-library that provides network-namespace-aware
`getaddrinfo`/`getnameinfo` on top of c-ares.  Read `README.md` and
`include/nsgai.h` for the design overview before making changes.

---

## Build and Test

```sh
# Configure (once)
cmake -S . -B build -DCMAKE_PREFIX_PATH=/home/balsup/c-ares/main.install/usr

# Build
cmake --build build

# Test (all tests, with output on failure)
ctest --test-dir build --output-on-failure

# Format sources in-place
cmake --build build --target format

# Check format without modifying (used by CI)
ctest --test-dir build -R nsgai_format_check

# Generate Doxygen HTML
cmake --build build --target docs
# Output: build/docs/html/index.html
```

`LD_LIBRARY_PATH=/home/balsup/c-ares/main.install/usr/lib64:build` is
required at runtime because c-ares is staged-installed, not system-wide.

The container test workflow (`scripts/run-container-tests.sh`) writes its
build tree into `build-container/` — **never** configure a host build
there.  Do not attempt to reuse that directory for a local build.

---

## Compiler Warnings Policy

All warnings are treated as errors (`-Werror`).  The full flag set is:

```
-Wall -Wextra -Werror -Wswitch-enum -Wno-unused-parameter
```

### `switch` on an enumeration — no `default` label allowed

`-Wswitch-enum` requires every enumerator to appear as a `case` label,
even when a `default` label is present.  The approved pattern for handling
"bad data" or "don't-care" cases is to **preset** a return/result variable
before the `switch`, then list every enumerator explicitly:

```c
int
nsgai_status_to_eai(ares_status_t s)
{
    int eai = EAI_FAIL; /* preset: covers unknown/infrastructure codes */
    switch (s) {
        case ARES_SUCCESS:   eai = 0;          break;
        case ARES_ENODATA:   eai = EAI_NONAME; break;
        /* ... all other enumerators ... */
        case ARES_ENOSERVER: break; /* EAI_FAIL (preset) */
    }
    return eai;
}
```

- Never add a `default:` label to a `switch` on an enum type.
- If c-ares (or any other dependency) adds a new enum value, the build
  will fail until the new case is handled explicitly.

---

## Code Style

Formatting is enforced by `.clang-format` (run `format` target before
committing).  The conventions below are **not** mechanically enforced and
must be followed manually.

### Declaration style (BSD / kernel style)

Return type on its own line; function name starts column 0:

```c
static int
compute_ns_key(const nsgai_ns_t *ns, dev_t *dev, ino_t *ino)
{
```

### Always brace `if`/`for`/`while` bodies

Even single-statement bodies require braces.  `clang-format`'s
`InsertBraces: true` enforces this on reformat, but write them explicitly
in new code.

### `case` labels are indented inside `switch`

```c
switch (s) {
    case ARES_SUCCESS:
        return 0;
    default:
        return EAI_FAIL;
}
```

### No `extern` prototype declarations in `.c` files

Cross-unit declarations belong in headers only.  If a symbol is needed
from another translation unit, include the relevant header.

### GNU shorthand ternary for null-coalescing

Prefer `a ?: b` over `a ? a : b` when the condition and the true branch
are the same expression:

```c
const char *proto = (hints && hints->ai_socktype == SOCK_STREAM) ? "tcp" : "udp";
// null-coalesce:
const char *name = ai->cnames->name ?: node;
```

### Preprocessor `#endif` / `#else` comments

Use the negated condition, not a description:

```c
#ifdef SO_BINDTODEVICE
    ...
#else    // ndef SO_BINDTODEVICE
    ...
#endif   // ndef SO_BINDTODEVICE
```

### `_GNU_SOURCE` is a compiler define, not a per-file macro

It is set globally in `CMakeLists.txt` via `add_compile_definitions`.
Do **not** add `#define _GNU_SOURCE` to individual source files.

---

## Error Handling

### pthread wrappers — abort vs. checked-return policy

All pthread calls go through helpers in `src/nsgai_diag.c` / declared in
`src/nsgai_internal.h`.  Follow the policy strictly:

| Operation | Helper | On failure |
|---|---|---|
| `pthread_mutex_lock` | `nsgai_pthread_mutex_lock_or_abort` | abort (state corrupt) |
| `pthread_mutex_unlock` | `nsgai_pthread_mutex_unlock_or_abort` | abort |
| `pthread_cond_wait` | `nsgai_pthread_cond_wait_or_abort` | abort |
| `pthread_cond_signal` | `nsgai_pthread_cond_signal_or_abort` | abort |
| `pthread_mutex_init` | `nsgai_pthread_mutex_init_checked` | return rc, caller propagates |
| `pthread_cond_init` | `nsgai_pthread_cond_init_checked` | return rc, caller propagates |
| `pthread_mutex_destroy` | `nsgai_pthread_mutex_destroy_checked` | log only, no abort |
| `pthread_cond_destroy` | `nsgai_pthread_cond_destroy_checked` | log only, no abort |

Never call `pthread_*` directly; never silently discard their return codes.

---

## Architecture

Three shared objects are built; keep their responsibilities separate:

| Library | Source | Role |
|---|---|---|
| `libnsgai.so` | `src/nsgai_*.c` | Public API + weak POSIX aliases |
| `libnsgai_preload.so` | `src/preload.c` | Unconditional POSIX aliases (LD_PRELOAD) |
| `libnss_nsgai.so.2` | `src/nss_nsgai.c` | NSS `hosts` backend |

**No fork.**  Namespace isolation is achieved by transiently
`setns(CLONE_NEWNET)` on c-ares' own event thread inside `cb_asocket`
(`src/nsgai_channel.c`).  The calling thread's netns is never touched by
the explicit `_ns`/`_nsfd` paths.

The implicit path (`nsgai_getaddrinfo`, etc.) never calls `setns` at all —
it inspects `/proc/thread-self/ns/net` to find per-ns config files only.

Channel objects are refcounted and LRU-cached in `nsgai_channel.c`.
Freshness is validated by re-`stat`ing `resolv.conf`/`hosts` on every
acquire.  Do not bypass the cache with direct `ares_init_options` calls.

---

## Documentation

All public symbols in `include/nsgai.h` and internal symbols in
`src/nsgai_internal.h` must have Doxygen-style `/** ... */` blocks with at
least `@brief`, `@param` (for each parameter), and `@return`.

Public functions are grouped into Topics via `@addtogroup`/`@{`/`@}` fencing
blocks **in the header**.  Do not use per-function `@ingroup` tags or
`@name`/`@{`/`@}` member-group fences in `.c` files — they override the
header's group assignment and produce incorrect Topics output.

Current groups: `nsgai_implicit`, `nsgai_explicit_name`,
`nsgai_explicit_fd`, `nsgai_posix`.  Add a new group in `nsgai.h` if a new
logical API area is introduced.

Static helpers in `.c` files need at minimum a `@brief` comment; full
`@param`/`@return` tags are encouraged for non-trivial helpers.

---

## Adding New Features

1. Public API changes go in `include/nsgai.h` with full Doxygen docs and
   placed in the correct `@addtogroup` fence.
2. Internal helpers go in `src/nsgai_internal.h` with Doxygen docs.
3. New source files must be added to the `nsgai_core` object library in
   `CMakeLists.txt` **and** to `test_channel_mocks` if any white-box test
   includes that `.c` file directly.
4. Cover new code paths with unit tests in `test/`.  Target: >90 % line
   coverage and >90 % branch coverage (measured via `coverage` target with
   `-DNSGAI_ENABLE_COVERAGE=ON`).
5. Run `cmake --build build --target format` and
   `ctest --test-dir build -R nsgai_format_check` before committing.
6. Commit messages follow Conventional Commits: `feat:`, `fix:`, `docs:`,
   `refactor:`, `test:`, `chore:`, etc.
