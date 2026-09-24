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

# nsgai Release Process

This document defines the source-only release process while GitHub Actions is
not yet available.

The project publishes only:

- The GitHub source archive for the release tag (`source.tgz` or equivalent).
- The instantiated SPDX source SBOM generated with the `source-sbom` target.

The project does not publish shared libraries, executables, RPMs, DEBs, or
other binary artifacts. Downstream distributions and users build the source
with their own toolchain and dependency packages.

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
  --baseline-tag v0.2.0
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
The baseline may be supplied as a committed library with `--baseline-lib`, or
built dynamically from the previous release tag in a temporary worktree with
`--baseline-tag`. Committing binary baselines is optional.

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
8. Create and push the release tag. GitHub provides the source archive for
  the tag.
9. Generate the instantiated SPDX SBOM from a clean release configuration:
  ```bash
  cmake -S . -B build-release \
    -DCMAKE_PREFIX_PATH=/path/to/c-ares \
    -DNSGAI_SPDX_NAMESPACE_BASE=https://github.com/cisco-open/nsgai/spdx \
    -DNSGAI_SPDX_SOURCE_DOWNLOAD_LOCATION=https://github.com/cisco-open/nsgai/archive/refs/tags/vX.Y.Z.tar.gz \
    -DNSGAI_SPDX_CREATED=YYYY-MM-DDThh:mm:ssZ
  cmake --build build-release --target source-sbom
  ```
10. Attach the generated `build-release/sbom/nsgai-X.Y.Z-source.spdx.json`
   to the GitHub release alongside the source archive.

## Post-release development reset

After the release tag, source archive, release notes, and SBOM have been
published, start the next development cycle in a separate commit. Do not make
this change before creating the release tag or generating release assets.

For the next planned version `A.B.C`, update both version sources:

1. In `CMakeLists.txt`, set `project(VERSION A.B.C)` and
  `NSGAI_VERSION_SUFFIX` to `"-dev"`.
2. In `include/nsgai.h`, keep the major/minor/patch macros at `A`, `B`, and
  `C`, set `NSGAI_VERSION_SUFFIX` to `"-dev"`, and set
  `NSGAI_VERSION_STRING` to `"A.B.C-dev"`.
3. Run the normal build, tests, and formatting checks.
4. Commit and push the development-version change to `main`.

The release tag and its generated SBOM remain tied to the release version;
the `-dev` commit is the starting point for the next release cycle.

## Standard tooling guidance

Use established ABI tooling rather than custom hash databases.

Recommended:

- `abidiff` (libabigail) for ABI compatibility analysis.
- Linker version scripts for symbol visibility/versioning.
- SONAME major bumps for ABI breaks.

A custom YAML hash database for APIs is optional metadata, not a substitute for ABI tooling.

## Downstream packaging

Distribution maintainers may fetch the source archive and build their own
packages for their operating system. The CMake install rules provide the
standard installation layout; package ownership, compiler flags, dependency
versions, and resulting binaries belong to the downstream distribution.

For local or downstream package generation, CPack remains enabled:

```bash
cmake --build build
cd build
cpack -G RPM  # Fedora/RHEL/openSUSE package
cpack -G DEB  # Debian/Ubuntu package
```

These are downstream or developer outputs, not Cisco release artifacts. The
`TGZ` CPack output is an install-tree package, not the canonical source
archive; releases use the source archive generated from the GitHub tag.

### For Yocto/OpenEmbedded

The project supports `cmake install` with a standard directory layout suitable
for Yocto bitbake recipes:

```bash
mkdir -p install_root
cmake --install build --prefix install_root
# Creates: install_root/bin/, install_root/include/, install_root/lib64/
```

Recipes should fetch the release source archive and build/package it within
the distribution's own infrastructure.
