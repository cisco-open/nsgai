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
# Opt in to the in-tree git hooks (currently: clang-format pre-commit).
# Run once per clone.  Idempotent.

set -euo pipefail

root="$(git rev-parse --show-toplevel)"
git -C "$root" config core.hooksPath scripts/hooks
echo "Hooks enabled: core.hooksPath = scripts/hooks"
echo "(bypass any hook for one commit with: git commit --no-verify)"
