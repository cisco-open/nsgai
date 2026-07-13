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
# Integration test for nsgai using real network namespaces.
#
# Requirements:
#   - run as root (CAP_NET_ADMIN + CAP_SYS_ADMIN)
#   - `ip` from iproute2
#
# This test:
#   1. Creates a fresh netns `nsgai_it_$$`
#   2. Brings up a loopback inside it
#   3. Installs /etc/netns/<ns>/{resolv.conf,hosts} with a known host entry
#   4. Uses nsgai_adig in three ways and checks each prints the expected addr:
#        a) -n <ns>                       (explicit by name)
#        b) `ip netns exec <ns> ADIG`     (implicit - current thread netns)
#        c) -f <fd> via exec              (explicit by fd)
#   5. Confirms a lookup *outside* the ns does NOT see the host entry.
#
# Cleanup runs unconditionally via trap.
#
# Usage: integration_netns.sh /path/to/nsgai_adig

set -euo pipefail

ADIG="${1:-}"
if [[ -z "$ADIG" || ! -x "$ADIG" ]]; then
    echo "usage: $0 /path/to/nsgai_adig" >&2
    exit 2
fi

if [[ $EUID -ne 0 ]]; then
    echo "SKIP: integration test requires root" >&2
    exit 77   # CTest "skip" exit code if configured; treat as skipped
fi
IP="$(command -v ip || true)"
if [[ -z "$IP" || ! -x "$IP" ]]; then
  echo "SKIP: 'ip' not found" >&2
  exit 77
fi

NS="nsgai_it_$$"
NS_ETC="/etc/netns/${NS}"
HOST="nsgai-it.invalid"
EXPECT_V4="127.42.42.42"

cleanup() {
    set +e
    "${IP}" netns del "${NS}" 2>/dev/null
    rm -rf "${NS_ETC}"
}
trap cleanup EXIT

# Make sure we use the freshly-built libs.
if [[ -n "${NSGAI_LIB_DIR:-}" ]]; then
    export LD_LIBRARY_PATH="${NSGAI_LIB_DIR}:${LD_LIBRARY_PATH:-}"
fi

mkdir -p "${NS_ETC}"
cat > "${NS_ETC}/hosts" <<EOF
${EXPECT_V4}   ${HOST}
EOF
# Minimal resolv.conf - we only need the hosts file to resolve, but c-ares
# expects a usable resolvconf.
cat > "${NS_ETC}/resolv.conf" <<EOF
nameserver 127.0.0.1
options timeout:1 attempts:1
EOF

"${IP}" netns add "${NS}"
nsenter --net="/var/run/netns/${NS}" -- "${IP}" link set lo up

check() {
    local label="$1"; shift
    local out
    out="$("$@" 2>&1)" || { echo "FAIL[$label] command failed:"; echo "$out"; exit 1; }
    if ! grep -q "${EXPECT_V4}" <<<"$out"; then
        echo "FAIL[$label]: expected ${EXPECT_V4} in output:"
        echo "$out"
        exit 1
    fi
    echo "PASS[$label]"
}

# (a) explicit by name
check "explicit-name" "$ADIG" -4 -n "$NS" "$HOST"

# (b) implicit: run the binary *inside* the ns; nsgai must auto-detect
#     /proc/thread-self/ns/net and pick up /etc/netns/<ns>/hosts.
#     Use nsenter rather than `ip netns exec` so we don't try to
#     remount /sys (forbidden inside a rootless user-namespace, which
#     is what the containerised test runner uses).
check "implicit-current-ns" nsenter --net="/var/run/netns/${NS}" -- \
    env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" "$ADIG" -4 "$HOST"

# (c) explicit by fd (open /var/run/netns/<ns> and pass as fd 3)
NS_FD_PATH="/var/run/netns/${NS}"
check "explicit-fd"   bash -c "exec 3<'${NS_FD_PATH}'; '${ADIG}' -4 -f 3 '${HOST}'"

# Negative: outside the ns, host must NOT resolve.
out_default="$("$ADIG" -4 "$HOST" 2>&1 || true)"
if grep -q "${EXPECT_V4}" <<<"$out_default"; then
    echo "FAIL[isolation]: default ns unexpectedly resolved ${HOST}:"
    echo "$out_default"
    exit 1
fi
echo "PASS[isolation]"

echo "All netns integration checks passed."
