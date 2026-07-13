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
# Verify that libnsgai lookup policy is honored per namespace, with both
# /etc/netns/<ns>/hosts and per-namespace DNS configured to return different
# addresses for the same hostname.
#
# Usage:
#   integration_policy_netns.sh /path/to/nsgai_adig
#
# Required:
#   - root (CAP_NET_ADMIN + CAP_SYS_ADMIN)
#   - ip from iproute2
#   - coredns on PATH, or in $COREDNS, or in $NSGAI_BUILD_DIR/coredns

set -euo pipefail

ADIG="${1:-}"
HOST="shared.nsgai.test"
NUM_NS=3
NAMESERVER_IP="127.0.0.2"

if [[ -z "$ADIG" || ! -x "$ADIG" ]]; then
    echo "usage: $0 /path/to/nsgai_adig" >&2
    exit 2
fi

if [[ $EUID -ne 0 ]]; then
    echo "SKIP: integration policy test requires root" >&2
    exit 77
fi
IP="$(command -v ip || true)"
if [[ -z "$IP" || ! -x "$IP" ]]; then
  echo "SKIP: 'ip' not found" >&2
  exit 77
fi

COREDNS_BIN="${COREDNS:-}"
if [[ -z "$COREDNS_BIN" ]]; then
    if command -v coredns >/dev/null 2>&1; then
        COREDNS_BIN="$(command -v coredns)"
    elif [[ -n "${NSGAI_BUILD_DIR:-}" && -x "${NSGAI_BUILD_DIR}/coredns" ]]; then
        COREDNS_BIN="${NSGAI_BUILD_DIR}/coredns"
    fi
fi
if [[ -z "$COREDNS_BIN" || ! -x "$COREDNS_BIN" ]]; then
    echo "SKIP: coredns not found. Run test/fetch_coredns.sh or set \$COREDNS." >&2
    exit 77
fi

WORK="$(mktemp -d -t nsgai_policy_it.XXXXXX)"
declare -a NSNAMES
declare -a CORE_PIDS

cleanup() {
    set +e
    for pid in "${CORE_PIDS[@]:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null
    done
    for ns in "${NSNAMES[@]:-}"; do
        "${IP}" netns del "$ns" 2>/dev/null
        rm -rf "/etc/netns/${ns}"
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

if [[ -n "${NSGAI_LIB_DIR:-}" ]]; then
    export LD_LIBRARY_PATH="${NSGAI_LIB_DIR}:${LD_LIBRARY_PATH:-}"
fi

check_lookup() {
    local label="$1"
    local expected="$2"
    shift 2
    local out

    out="$("$@" 2>&1)"
    if ! grep -qF "$expected" <<<"$out"; then
        echo "FAIL[$label]: expected $expected, got:" >&2
        echo "$out" >&2
        exit 1
    fi
    echo "PASS[$label] -> $expected"
}

for k in $(seq 1 "$NUM_NS"); do
    ns="nsgai_policy${k}_$$"
    dns_addr="10.88.${k}.1"
    hosts_addr="192.0.2.${k}"
    NSNAMES+=("$ns")

    nsdir="${WORK}/ns${k}"
    mkdir -p "$nsdir"

    cat > "${nsdir}/db.shared" <<EOF
\$ORIGIN nsgai.test.
\$TTL 30
@         IN SOA  ns.nsgai.test. admin.nsgai.test. (1 60 60 600 30)
@         IN NS   ns.nsgai.test.
ns        IN A    ${NAMESERVER_IP}
shared    IN A    ${dns_addr}
EOF

    cat > "${nsdir}/Corefile" <<EOF
. {
    bind ${NAMESERVER_IP}
    file ${nsdir}/db.shared nsgai.test
    log
    errors
}
EOF

    etcdir="/etc/netns/${ns}"
    mkdir -p "$etcdir"
    cat > "${etcdir}/resolv.conf" <<EOF
nameserver ${NAMESERVER_IP}
options timeout:1 attempts:2 ndots:0
EOF
    cat > "${etcdir}/hosts" <<EOF
127.0.0.1 localhost
${hosts_addr} ${HOST}
EOF

    "${IP}" netns add "$ns"
    nsenter --net="/var/run/netns/${ns}" -- "${IP}" link set lo up
    nsenter --net="/var/run/netns/${ns}" -- \
        "$COREDNS_BIN" -conf "${nsdir}/Corefile" \
        >"${nsdir}/coredns.log" 2>&1 &
    CORE_PIDS+=("$!")

    tries=50
    while (( tries-- > 0 )); do
        if "$ADIG" -4 -n "$ns" -p dns-only "$HOST" 2>&1 | grep -qF "$dns_addr"; then
            break
        fi
        sleep 0.1
    done
    if (( tries < 0 )); then
        echo "FAIL: coredns in $ns did not become ready" >&2
        cat "${nsdir}/coredns.log" >&2 || true
        exit 1
    fi

    check_lookup "$ns implicit hosts-first" "$hosts_addr" \
        nsenter --net="/var/run/netns/${ns}" -- \
        env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
        "$ADIG" -4 -p hosts-first "$HOST"

    check_lookup "$ns implicit dns-first" "$dns_addr" \
        nsenter --net="/var/run/netns/${ns}" -- \
        env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
        "$ADIG" -4 -p dns-first "$HOST"

    check_lookup "$ns by-name hosts-only" "$hosts_addr" \
        "$ADIG" -4 -n "$ns" -p hosts-only "$HOST"

    check_lookup "$ns by-name dns-only" "$dns_addr" \
        "$ADIG" -4 -n "$ns" -p dns-only "$HOST"

    check_lookup "$ns by-fd hosts-first" "$hosts_addr" \
        bash -c "exec 9<'/var/run/netns/${ns}'; '${ADIG}' -4 -f 9 -p hosts-first '${HOST}'"

    check_lookup "$ns by-fd dns-first" "$dns_addr" \
        bash -c "exec 9<'/var/run/netns/${ns}'; '${ADIG}' -4 -f 9 -p dns-first '${HOST}'"
done

echo "All policy netns integration checks passed."
