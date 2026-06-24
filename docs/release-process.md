# nsgai Release Process

This document defines the manual release process while GitHub Actions is not yet available.

## Goals

- Keep API and ABI stable across non-major releases.
- Make release compatibility checks repeatable on a developer machine.
- Ensure intentional version bumps when compatibility changes.

## Current compatibility mechanisms in this repository

- Project version and SONAME for libnsgai are set in CMake.
  - `VERSION` is `${PROJECT_VERSION}`
  - `SOVERSION` is `${PROJECT_VERSION_MAJOR}`
- Exported symbols are controlled by the linker version script at `src/nsgai.map`.
  - Current symbol namespace: `NSGAI_0`
  - Non-public symbols are hidden via `local: *;`

## API vs ABI policy

- API is the public header contract in `include/nsgai.h`.
- ABI is the binary contract exposed by `libnsgai.so`.

Versioning rules:

- Patch (`X.Y.Z`): bug fixes only, no API/ABI break.
- Minor (`X.Y.0`): additive API/ABI changes only.
- Major (`X.0.0`): incompatible API/ABI changes allowed.

SONAME rule:

- Any ABI break requires a major bump and SONAME bump.

## Symbol-versioning rule

When adding new public functions without breaking old ABI:

1. Keep old symbols in existing map node (`NSGAI_0`).
2. Add a new node (example: `NSGAI_1`) for new exports.
3. Chain it to the previous node for inheritance.

Example pattern:

```ld
NSGAI_0 {
    global:
        old_symbol;
    local:
        *;
};

NSGAI_1 {
    global:
        new_symbol;
} NSGAI_0;
```

Never remove or retarget existing symbols in a non-major release.

## Manual release gates

Run all release gates from repository root:

```sh
scripts/release-check.sh \
  --cmake-prefix-path /home/balsup/c-ares/main.install/usr \
  --cares-libdir /home/balsup/c-ares/main.install/usr/lib64 \
  --baseline-lib abi/baselines/v0.1.0/libnsgai.so.0.1.0
```

The script performs:

1. Configure and build.
2. Full CTest run.
3. Exported-symbol check: `libnsgai` exports exactly the symbols listed in `src/nsgai.map`.
4. ABI diff check with `abidiff` against a baseline release library (if provided).

## Baseline management

Store one baseline shared object per release tag, for example:

- `abi/baselines/v0.1.0/libnsgai.so.0.1.0`
- `abi/baselines/v0.2.0/libnsgai.so.0.2.0`

For each new release candidate, compare against the latest released baseline.

## Required release checklist

1. Decide version bump type (patch/minor/major) from actual API/ABI impact.
2. Update project version in `CMakeLists.txt`.
3. Update `src/nsgai.map` only as needed:
   - additive symbols: add to a new map node
   - breaking changes: major bump and migration notes
4. Run `scripts/release-check.sh` with baseline comparison.
5. Review ABI report (`build/release/abidiff.txt`) if present.
6. Verify namespace resolver-file semantics are documented clearly in
  `README.md`:
  - `/etc/netns/<name>/resolv.conf` falls back to `/etc/resolv.conf`
  - `/etc/netns/<name>/hosts` falls back to `/etc/hosts`
  - fallback is per file (partial override supported)
7. Update `README.md` and release notes with API/ABI changes.
8. Generate packages (optional for manual distribution):
   ```bash
   cd build
   cpack -G TGZ    # source tarball (recommended for Yocto/OpenEmbedded)
   cpack -G RPM    # binary RPM (Fedora/RHEL/openSUSE)
   cpack -G DEB    # binary DEB (Debian/Ubuntu)
   ```
9. Tag release.

## Standard tooling guidance

Use established ABI tooling rather than custom hash databases.

Recommended:

- `abidiff` (libabigail) for ABI compatibility analysis.
- Linker version scripts for symbol visibility/versioning.
- SONAME major bumps for ABI breaks.

A custom YAML hash database for APIs is optional metadata, not a substitute for ABI tooling.

## Packaging and distribution

### For Yocto/OpenEmbedded

The project supports `cmake install` with standard directory layout suitable for Yocto bitbake recipes:

```bash
mkdir -p install_root
cmake --install build --prefix install_root
# Creates: install_root/bin/, install_root/include/, install_root/lib64/
```

Bitbake recipes can fetch the source tarball and build/package according to the recipe's configuration:

```bash
cpack -G TGZ      # generates nsgai-X.Y.Z-Linux.tar.gz
```

### For Fedora/RHEL/openSUSE

```bash
cpack -G RPM      # generates nsgai-X.Y.Z-Linux.rpm
```

### For Debian/Ubuntu

```bash
cpack -G DEB      # generates nsgai-X.Y.Z-Linux.deb
```

Packages are created via CMake's CPack, which generates source and binary distributions. 
See [CPack documentation](https://cmake.org/cmake/help/latest/module/CPack.html) for customization.
