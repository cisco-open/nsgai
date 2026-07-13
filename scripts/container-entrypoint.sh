#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# In-container entrypoint: configure with coverage + netns tests enabled,
# build, run the full ctest suite, then emit an HTML coverage report.
#
# Builds into /src/build-container/ so the developer's normal host build/
# tree is untouched. The host-side wrapper (run-container-tests.sh) bind-
# mounts the source tree read-write and arranges UID mapping so that
# files created here are owned by the invoking user on the host.

set -euo pipefail

cd /src

BUILD_DIR="${NSGAI_BUILD_DIR:-build-container}"

# Ensure files we create end up owned by the developer on the host when
# running rootful (sudo) podman. HOST_UID/HOST_GID are exported by the
# launcher script. Harmless when both are 0 or unset.
cleanup_perms() {
    local host_uid="${HOST_UID:-0}"
    local host_gid="${HOST_GID:-$host_uid}"
    if [[ "$(id -u)" -eq 0 && "$host_uid" -ne 0 ]]; then
        echo "==> Re-owning $BUILD_DIR to $host_uid:$host_gid"
        chown -R "$host_uid:$host_gid" "$BUILD_DIR" 2>/dev/null || true
    fi
}
trap cleanup_perms EXIT

# This build tree is dedicated to the container workflow. Recreate it on
# each run so compiler/toolchain changes in the image cannot leave behind
# stale objects or coverage metadata that break gcov/gcovr processing.
rm -rf "$BUILD_DIR"

echo "==> Configuring (coverage + netns tests)"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNSGAI_ENABLE_COVERAGE=ON \
    -DNSGAI_ENABLE_NETNS_TESTS=ON

echo "==> Building"
cmake --build "$BUILD_DIR" -j"$(nproc)"

# fetch_coredns.sh is a no-op if a binary already exists; symlink the
# image-baked one into the build dir so it picks it up without network.
if [[ ! -x "$BUILD_DIR/coredns" && -x /usr/local/bin/coredns ]]; then
    ln -sf /usr/local/bin/coredns "$BUILD_DIR/coredns"
fi

echo "==> Cleaning stale coverage artifacts"
cmake --build "$BUILD_DIR" --target coverage-clean

echo "==> Running coverage target (ctest + report)"
cmake --build "$BUILD_DIR" --target coverage

echo
echo "==> Coverage report: $BUILD_DIR/coverage/html/index.html"
