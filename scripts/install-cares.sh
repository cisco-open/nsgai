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
# Shared helper used by every per-distro Dockerfile (see docker/) to
# provision c-ares: use the distro package if it is new enough, otherwise
# build the pinned upstream ref from source. Keeping this logic in one
# script means every platform image applies the same version policy
# instead of hand-rolling distro-specific conditionals.
#
# Usage:
#   install-cares.sh --prefix DIR [--ref REF] [--min-version X.Y.Z] [--repo URL]
#
# On success, either:
#   - the distro package satisfied --min-version and nothing further is
#     needed (script exits 0, --prefix is left untouched), or
#   - c-ares was built from source and installed to --prefix, with its
#     lib dir registered via ldconfig.

set -euo pipefail

prefix=/opt/c-ares
ref=main
min_version=1.27.0
repo=https://github.com/c-ares/c-ares.git

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        --ref) ref="$2"; shift 2 ;;
        --min-version) min_version="$2"; shift 2 ;;
        --repo) repo="$2"; shift 2 ;;
        -h|--help)
            sed -n '7,26p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "install-cares.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

version_ge() {
    # true if $1 >= $2
    [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n1)" == "$2" ]]
}

if pkg-config --exists libcares 2>/dev/null; then
    sys_version="$(pkg-config --modversion libcares)"
    if version_ge "$sys_version" "$min_version"; then
        echo "install-cares.sh: distro libcares $sys_version >= $min_version, using it"
        exit 0
    fi
    echo "install-cares.sh: distro libcares $sys_version < $min_version, building from source"
else
    echo "install-cares.sh: no distro libcares.pc found, building from source"
fi

src_dir="$(mktemp -d)"
trap 'rm -rf "$src_dir"' EXIT

git clone --depth 50 --branch "$ref" "$repo" "$src_dir"
cmake -S "$src_dir" -B "$src_dir/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCARES_STATIC=OFF \
    -DCARES_SHARED=ON \
    -DCARES_BUILD_TESTS=OFF \
    -DCARES_BUILD_TOOLS=OFF
cmake --build "$src_dir/build" -j"$(nproc)"
cmake --install "$src_dir/build"

if [[ -w /etc/ld.so.conf.d ]]; then
    {
        echo "$prefix/lib64"
        echo "$prefix/lib"
    } > /etc/ld.so.conf.d/c-ares.conf
    ldconfig
fi

echo "install-cares.sh: built c-ares ($ref) from source into $prefix"
