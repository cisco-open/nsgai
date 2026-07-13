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
