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
# Build an nsgai test image for one (or every) supported platform and run
# the full test suite (including the root-requiring netns integration
# tests) inside a privileged container. Coverage is collected and an HTML
# report is left on the host at
# build-container-<platform>-<arch>/coverage/html/index.html. See
# docker/README.md for the platform matrix.
#
# Usage:
#   scripts/run-container-tests.sh [--platform NAME | --all] [--arch ARCH] [--rebuild] [--shell] [--sudo]
#
#   --platform NAME  Platform to build/test (default: fedora). Must match
#                     a docker/Dockerfile.<NAME> file.
#   --all            Run every platform in docker/Dockerfile.* sequentially
#                     instead of a single one. Ignores --platform/--shell.
#   --arch ARCH      Architecture tag/build-dir suffix (default: host arch,
#                     normalized to amd64 or arm64). The Dockerfiles
#                     themselves are already arch-agnostic (dnf/apt resolve
#                     the right packages, and the CoreDNS fetch branches on
#                     `uname -m`); this flag only affects naming, so that
#                     building the same platform for two architectures
#                     (e.g. via emulation, or a shared cache) never
#                     collides on image tag or build directory.
#   --rebuild        Force `podman build --no-cache` even if the image exists.
#   --shell          Drop into an interactive shell instead of running tests.
#   --sudo           Run podman via sudo (rootful). Not needed under rootless
#                     podman now that the netns tests use nsenter instead of
#                     `ip netns exec` — pass this only if you want the extra
#                     isolation of a rootful container.
#
# Environment:
#   NSGAI_CONTAINER_RUNTIME  podman (default) or docker
#   NSGAI_TEST_IMAGE         image tag override (default: nsgai-test:<platform>-<arch>)
#   NSGAI_SUDO               sudo command (default: sudo) when --sudo
#                            is in effect.
#
# Notes:
#   - Runs the container --privileged so it has CAP_NET_ADMIN/SYS_ADMIN
#     inside its own user namespace. Under rootless podman this stays
#     confined to the user namespace and does NOT grant root on the host.
#   - The source tree is bind-mounted read-write; build artefacts land in
#     ./build-container-<platform>-<arch>/ on the host (separate per
#     platform and architecture, and separate from your dev build/).

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME="${NSGAI_CONTAINER_RUNTIME:-podman}"

PLATFORM="fedora"
ALL=0
REBUILD=0
SHELL_MODE=0
USE_SUDO=0
ARCH=""
for arg in "$@"; do
    case "$arg" in
        --platform=*) PLATFORM="${arg#*=}" ;;
        --platform)   PLATFORM="__NEXT__" ;;
        --arch=*)     ARCH="${arg#*=}" ;;
        --arch)       ARCH="__NEXT__" ;;
        --all)        ALL=1 ;;
        --rebuild)    REBUILD=1 ;;
        --shell)      SHELL_MODE=1 ;;
        --sudo)       USE_SUDO=1 ;;
        -h|--help)
            sed -n '7,48p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            if [[ "$PLATFORM" == "__NEXT__" ]]; then
                PLATFORM="$arg"
            elif [[ "$ARCH" == "__NEXT__" ]]; then
                ARCH="$arg"
            else
                echo "unknown argument: $arg" >&2
                exit 2
            fi
            ;;
    esac
done
if [[ "$PLATFORM" == "__NEXT__" ]]; then
    echo "error: --platform requires a value" >&2
    exit 2
fi
if [[ "$ARCH" == "__NEXT__" ]]; then
    echo "error: --arch requires a value" >&2
    exit 2
fi

# Normalize `uname -m` (or a user-supplied --arch) to the amd64/arm64
# naming convention used by container tooling.
normalize_arch() {
    case "$1" in
        x86_64|amd64)  echo amd64 ;;
        aarch64|arm64) echo arm64 ;;
        armv7l|armhf)  echo armv7 ;;
        *)             echo "$1" ;;
    esac
}
ARCH="$(normalize_arch "${ARCH:-$(uname -m)}")"

if ! command -v "$RUNTIME" >/dev/null 2>&1; then
    echo "error: container runtime '$RUNTIME' not found on PATH" >&2
    exit 1
fi

SUDO=""
if [[ $USE_SUDO -eq 1 && $EUID -ne 0 ]]; then
    SUDO="${NSGAI_SUDO:-sudo}"
fi

PODMAN=("$RUNTIME")
[[ -n "$SUDO" ]] && PODMAN=("$SUDO" "$RUNTIME")

cd "$ROOT"

run_one_platform() {
    local platform="$1"
    local dockerfile="docker/Dockerfile.${platform}"
    local image="${NSGAI_TEST_IMAGE:-nsgai-test:${platform}-${ARCH}}"
    local build_dir="build-container-${platform}-${ARCH}"

    if [[ ! -f "$dockerfile" ]]; then
        echo "error: no such platform '$platform' (expected $dockerfile)" >&2
        echo "       see docker/README.md for the supported platform list" >&2
        exit 2
    fi

    local build_args=()
    [[ $REBUILD -eq 1 ]] && build_args+=(--no-cache)

    if [[ $REBUILD -eq 1 ]] || ! "${PODMAN[@]}" image exists "$image" >/dev/null 2>&1; then
        echo "==> [$platform/$ARCH] Building image $image (runtime: ${PODMAN[*]})"
        "${PODMAN[@]}" build "${build_args[@]}" -t "$image" -f "$dockerfile" .
    else
        echo "==> [$platform/$ARCH] Reusing existing image $image (use --rebuild to force)"
    fi

    mkdir -p "$ROOT/$build_dir"

    local run_args=(
        --rm
        --privileged
        --security-opt label=disable
        -v "$ROOT":/src
        -w /src
        -e HOST_UID="$(id -u)"
        -e HOST_GID="$(id -g)"
        -e NSGAI_BUILD_DIR="$build_dir"
    )
    # Interactive shell only when a TTY is attached.
    if [[ -t 0 && -t 1 ]]; then
        run_args+=(-it)
    fi

    if [[ $SHELL_MODE -eq 1 ]]; then
        exec "${PODMAN[@]}" run "${run_args[@]}" "$image" bash
    fi

    echo "==> [$platform/$ARCH] Running tests in container"
    "${PODMAN[@]}" run "${run_args[@]}" "$image" \
        /src/scripts/container-entrypoint.sh

    echo "==> [$platform/$ARCH] Done. Coverage report on host:"
    echo "    $ROOT/$build_dir/coverage/html/index.html"
}

if [[ $ALL -eq 1 ]]; then
    shopt -s nullglob
    platforms=()
    for f in docker/Dockerfile.*; do
        platforms+=("${f#docker/Dockerfile.}")
    done
    shopt -u nullglob
    if [[ ${#platforms[@]} -eq 0 ]]; then
        echo "error: no docker/Dockerfile.* files found" >&2
        exit 1
    fi
    echo "==> Running matrix across platforms (arch=$ARCH): ${platforms[*]}"
    for p in "${platforms[@]}"; do
        run_one_platform "$p"
    done
else
    run_one_platform "$PLATFORM"
fi
