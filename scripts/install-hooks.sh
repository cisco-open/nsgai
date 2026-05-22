#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Opt in to the in-tree git hooks (currently: clang-format pre-commit).
# Run once per clone.  Idempotent.

set -euo pipefail

root="$(git rev-parse --show-toplevel)"
git -C "$root" config core.hooksPath scripts/hooks
echo "Hooks enabled: core.hooksPath = scripts/hooks"
echo "(bypass any hook for one commit with: git commit --no-verify)"
