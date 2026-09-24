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
# Run manual release gates for libnsgai:
#   1) configure + build
#   2) test suite (ctest)
#   3) exported-symbol check against src/nsgai.map
#   4) optional ABI diff against a release baseline (abidiff)

set -euo pipefail

usage() {
        cat <<'EOF'
Usage: scripts/release-check.sh [options]

Options:
  --build-dir DIR           Build tree path (default: build)
  --cmake-prefix-path PATH  Value for -DCMAKE_PREFIX_PATH=...
  --cares-libdir DIR        c-ares runtime library dir for tests
  --baseline-lib FILE       Previous release libnsgai.so for ABI diff
  --baseline-tag TAG        Build the previous release from a temporary worktree
  --skip-tests              Skip ctest execution
  --skip-symbols            Skip symbol-map gate
  --skip-abi                Skip ABI gate (abidiff)
  -h, --help                Show help

Examples:
  scripts/release-check.sh \
    --cmake-prefix-path /home/balsup/c-ares/main.install/usr \
    --cares-libdir /home/balsup/c-ares/main.install/usr/lib64

  scripts/release-check.sh \
    --baseline-lib abi/baselines/v0.1.0/libnsgai.so.0.1.0 \
    --cmake-prefix-path /home/balsup/c-ares/main.install/usr \
    --cares-libdir /home/balsup/c-ares/main.install/usr/lib64

  scripts/release-check.sh \
    --baseline-tag v0.2.0 \
    --cmake-prefix-path /home/balsup/c-ares/main.install/usr \
    --cares-libdir /home/balsup/c-ares/main.install/usr/lib64
EOF
}

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool not found in PATH: $1" >&2
        exit 1
    fi
}

build_dir="build"
cmake_prefix_path="${CMAKE_PREFIX_PATH:-}"
cares_libdir=""
baseline_lib=""
baseline_tag=""
skip_tests=0
skip_symbols=0
skip_abi=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --cmake-prefix-path)
            cmake_prefix_path="$2"
            shift 2
            ;;
        --cares-libdir)
            cares_libdir="$2"
            shift 2
            ;;
        --baseline-lib)
            baseline_lib="$2"
            shift 2
            ;;
        --baseline-tag)
            baseline_tag="$2"
            shift 2
            ;;
        --skip-tests)
            skip_tests=1
            shift
            ;;
        --skip-symbols)
            skip_symbols=1
            shift
            ;;
        --skip-abi)
            skip_abi=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

require_tool cmake
require_tool ctest
require_tool nm
require_tool sed
require_tool awk
require_tool sort
require_tool comm
if [[ -n "$baseline_tag" ]]; then
    require_tool git
fi

if [[ -n "$baseline_lib" && -n "$baseline_tag" ]]; then
    echo "error: --baseline-lib and --baseline-tag are mutually exclusive" >&2
    exit 1
fi

root="$(git rev-parse --show-toplevel)"
cd "$root"

baseline_root=""
baseline_worktree=""
cleanup() {
    if [[ -n "$baseline_worktree" && -d "$baseline_worktree" ]]; then
        git worktree remove --force "$baseline_worktree" >/dev/null 2>&1 || true
    fi
    if [[ -n "$baseline_root" && -d "$baseline_root" ]]; then
        rm -rf "$baseline_root"
    fi
}
trap cleanup EXIT

configure_cmd=(cmake -S "$root" -B "$build_dir")
if [[ -n "$cmake_prefix_path" ]]; then
    configure_cmd+=("-DCMAKE_PREFIX_PATH=$cmake_prefix_path")
fi

echo "[1/4] Configure"
"${configure_cmd[@]}"

echo "[2/4] Build"
cmake --build "$build_dir"

ld_path="$root/$build_dir"
if [[ -n "$cares_libdir" ]]; then
    ld_path="$cares_libdir:$ld_path"
fi
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    ld_path="$ld_path:$LD_LIBRARY_PATH"
fi

if [[ $skip_tests -eq 0 ]]; then
    echo "[3/4] Test"
    LD_LIBRARY_PATH="$ld_path" ctest --test-dir "$build_dir" --output-on-failure
else
    echo "[3/4] Test: skipped"
fi

lib_path="$(find "$root/$build_dir" -maxdepth 1 -type f -name 'libnsgai.so*' | sort -V | tail -n1 || true)"
if [[ -z "$lib_path" ]]; then
    echo "error: could not locate built libnsgai shared object under $build_dir" >&2
    exit 1
fi

if [[ $skip_symbols -eq 0 ]]; then
    echo "[4/4] Symbol gate (version script consistency)"

    expected_file="$(mktemp)"
    actual_file="$(mktemp)"
    missing_file="$(mktemp)"
    extra_file="$(mktemp)"
    trap 'rm -f "$expected_file" "$actual_file" "$missing_file" "$extra_file"' EXIT

    sed -n '/global:/,/local:/p' "$root/src/nsgai.map" \
        | grep -E '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;' \
        | sed -E 's/^[[:space:]]*//; s/[[:space:]]*;[[:space:]]*$//' \
        | sort -u > "$expected_file"

    nm -D --defined-only "$lib_path" \
        | awk '$2 ~ /[TWBV]/ {print $3}' \
        | sed -E 's/@.*$//' \
        | sort -u > "$actual_file"

    comm -23 "$expected_file" "$actual_file" > "$missing_file"
    comm -13 "$expected_file" "$actual_file" > "$extra_file"

    if [[ -s "$missing_file" ]]; then
        echo "error: missing exported symbols from libnsgai:" >&2
        sed 's/^/  - /' "$missing_file" >&2
        exit 1
    fi

    if [[ -s "$extra_file" ]]; then
        echo "error: unexpected exported symbols found in libnsgai:" >&2
        sed 's/^/  - /' "$extra_file" >&2
        echo "hint: update src/nsgai.map or hide accidental exports" >&2
        exit 1
    fi

    echo "symbol gate: OK"
else
    echo "[4/4] Symbol gate: skipped"
fi

if [[ $skip_abi -eq 0 ]]; then
    if [[ -n "$baseline_tag" ]]; then
        baseline_root="$(mktemp -d "${TMPDIR:-/tmp}/nsgai-baseline.XXXXXX")"
        baseline_worktree="$baseline_root/worktree"
        baseline_build_dir="$baseline_root/build"

        echo "ABI gate: building baseline tag $baseline_tag in a temporary worktree"
        git worktree add --detach "$baseline_worktree" "$baseline_tag"

        baseline_configure_cmd=(
            cmake -S "$baseline_worktree" -B "$baseline_build_dir"
            -DNSGAI_BUILD_TESTS=OFF
            -DNSGAI_BUILD_PRELOAD=OFF
            -DNSGAI_BUILD_NSS=OFF
            -DNSGAI_BUILD_EXAMPLES=OFF
        )
        if [[ -n "$cmake_prefix_path" ]]; then
            baseline_configure_cmd+=("-DCMAKE_PREFIX_PATH=$cmake_prefix_path")
        fi
        "${baseline_configure_cmd[@]}"
        cmake --build "$baseline_build_dir" --target nsgai

        baseline_lib="$(find "$baseline_build_dir" -maxdepth 1 -type f \
            -name 'libnsgai.so*' | sort -V | tail -n1 || true)"
        if [[ -z "$baseline_lib" ]]; then
            echo "error: could not locate baseline libnsgai.so for tag $baseline_tag" >&2
            exit 1
        fi
    fi

    if [[ -z "$baseline_lib" ]]; then
        echo "ABI gate: skipped (no --baseline-lib or --baseline-tag provided)"
    else
        require_tool abidiff
        if [[ ! -f "$baseline_lib" ]]; then
            echo "error: baseline library not found: $baseline_lib" >&2
            exit 1
        fi

        report_dir="$root/$build_dir/release"
        report_file="$report_dir/abidiff.txt"
        mkdir -p "$report_dir"

        echo "ABI gate: comparing baseline -> current"
        set +e
        abidiff --leaf-changes-only "$baseline_lib" "$lib_path" > "$report_file" 2>&1
        rc=$?
        set -e

        case "$rc" in
            0)
                echo "ABI gate: OK"
                ;;
            1)
                echo "error: ABI compatibility check failed" >&2
                echo "see report: $report_file" >&2
                exit 1
                ;;
            *)
                echo "error: abidiff failed (exit=$rc)" >&2
                echo "see report: $report_file" >&2
                exit 1
                ;;
        esac
    fi
else
    echo "ABI gate: skipped"
fi

echo "release-check: all enabled gates passed"
