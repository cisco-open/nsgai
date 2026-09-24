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

# ABI Baselines

This directory may store reference shared objects for ABI compatibility checks
during release. A committed binary is optional; the release gate can build a
previous release tag in a temporary worktree instead.

## Organization

Store one baseline library per released version tag:

```
abi/baselines/v0.1.0/libnsgai.so.0.1.0
abi/baselines/v0.2.0/libnsgai.so.0.2.0
```

## Usage

When preparing a release candidate, compare the newly built `libnsgai.so` against the latest released baseline:

```sh
scripts/release-check.sh \
  --cmake-prefix-path /path/to/c-ares/install \
  --cares-libdir /path/to/c-ares/lib \
  --baseline-lib abi/baselines/v0.1.0/libnsgai.so.0.1.0
```

The script will:

1. Build the current tree.
2. Run the full test suite.
3. Validate exported symbols against `src/nsgai.map`.
4. Run `abidiff` to compare the baseline against the current build.
5. Write the ABI diff report to `build/release/abidiff.txt`.

To build the baseline dynamically from a previous tag without committing a
binary, use `--baseline-tag`:

```sh
scripts/release-check.sh \
  --baseline-tag v0.2.0 \
  --cmake-prefix-path /path/to/c-ares/install \
  --cares-libdir /path/to/c-ares/lib
```

The script creates a detached temporary worktree, builds only the library,
runs `abidiff`, and removes the worktree afterward.

## Baseline creation

If reproducible local baselines are preferred, extract the built library after
tagging a release and store it:

```sh
mkdir -p abi/baselines/vX.Y.Z
cp build/libnsgai.so.X.Y.Z abi/baselines/vX.Y.Z/
git add abi/baselines/vX.Y.Z/
```

Commit the baseline alongside the release tag. This is optional when using
`--baseline-tag`.

## ABI compatibility rules

- **Patch release** (X.Y.Z): no ABI changes expected. Baseline should be identical.
- **Minor release** (X.Y.0): additive ABI only. Baseline diff should show new symbols only, no removals or signature changes.
- **Major release** (X.0.0): ABI incompatibilities allowed. Document all breaking changes in release notes.

See [docs/release-process.md](../../docs/release-process.md) for full versioning policy.
