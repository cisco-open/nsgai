#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Build the nsgai test image and run the full test suite (including the
# root-requiring netns integration tests) inside a privileged container.
# Coverage is collected and an HTML report is left on the host at
# build-container/coverage/html/index.html.
#
# Usage:
#   scripts/run-container-tests.sh [--rebuild] [--shell] [--sudo]
#
#   --rebuild   Force `podman build --no-cache` even if the image exists.
#   --shell     Drop into an interactive shell instead of running tests.
#   --sudo      Run podman via sudo (rootful). Not needed under rootless
#                podman now that the netns tests use nsenter instead of
#                `ip netns exec` — pass this only if you want the extra
#                isolation of a rootful container.
#
# Environment:
#   NSGAI_CONTAINER_RUNTIME  podman (default) or docker
#   NSGAI_TEST_IMAGE         image tag (default: nsgai-test:latest)
#   NSGAI_SUDO               sudo command (default: sudo) when --sudo
#                            is in effect.
#
# Notes:
#   - Runs the container --privileged so it has CAP_NET_ADMIN/SYS_ADMIN
#     inside its own user namespace. Under rootless podman this stays
#     confined to the user namespace and does NOT grant root on the host.
#   - The source tree is bind-mounted read-write; build artefacts land
#     in ./build-container/ on the host (separate from your dev build/).

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME="${NSGAI_CONTAINER_RUNTIME:-podman}"
IMAGE="${NSGAI_TEST_IMAGE:-nsgai-test:latest}"

REBUILD=0
SHELL_MODE=0
USE_SUDO=0
for arg in "$@"; do
    case "$arg" in
        --rebuild) REBUILD=1 ;;
        --shell)   SHELL_MODE=1 ;;
        --sudo)    USE_SUDO=1 ;;
        -h|--help)
            sed -n '7,28p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

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

build_args=()
[[ $REBUILD -eq 1 ]] && build_args+=(--no-cache)

if [[ $REBUILD -eq 1 ]] || ! "${PODMAN[@]}" image exists "$IMAGE" >/dev/null 2>&1; then
    echo "==> Building image $IMAGE (runtime: ${PODMAN[*]})"
    "${PODMAN[@]}" build "${build_args[@]}" -t "$IMAGE" -f Dockerfile .
else
    echo "==> Reusing existing image $IMAGE (use --rebuild to force)"
fi

mkdir -p "$ROOT/build-container"

run_args=(
    --rm
    --privileged
    --security-opt label=disable
    -v "$ROOT":/src
    -w /src
    -e HOST_UID="$(id -u)"
    -e HOST_GID="$(id -g)"
)
# Interactive shell only when a TTY is attached.
if [[ -t 0 && -t 1 ]]; then
    run_args+=(-it)
fi

if [[ $SHELL_MODE -eq 1 ]]; then
    exec "${PODMAN[@]}" run "${run_args[@]}" "$IMAGE" bash
fi

echo "==> Running tests in container"
echo "==> Note: ./build-container is a container-only build tree; use ./build for host-side cmake --build/ctest"
"${PODMAN[@]}" run "${run_args[@]}" "$IMAGE" \
    /src/scripts/container-entrypoint.sh

echo
echo "==> Done. Coverage report on host:"
echo "    $ROOT/build-container/coverage/html/index.html"

