#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Reformat all first-party C sources with clang-format.
# Thin convenience wrapper; equivalent to:
#   cmake --build build --target format
#
# Use --check to only verify (exit non-zero on diff).

set -euo pipefail

root="$(git rev-parse --show-toplevel)"
cd "$root"

CF="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CF" >/dev/null 2>&1; then
    echo "$CF not found in PATH" >&2
    exit 1
fi

mode=fix
if [[ "${1:-}" == "--check" ]]; then mode=check; fi

mapfile -t files < <(git ls-files \
    'include/*.h' 'src/*.c' 'src/*.h' \
    'examples/*.c' 'examples/*.h' \
    'test/*.c' 'test/*.h')

case "$mode" in
    fix)   "$CF" -i --style=file "${files[@]}" ;;
    check) "$CF" --dry-run -Werror --style=file "${files[@]}" ;;
esac
