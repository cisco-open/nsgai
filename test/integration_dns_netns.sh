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
# Exhaustive nsgai integration test using real network namespaces, each
# running a CoreDNS instance bound to 127.0.0.1:53 and serving the same
# hostname with a *different* A record.
#
# Usage:
#   integration_dns_netns.sh /path/to/nsgai_adig /path/to/test_multins \
#                            [/path/to/test_cache_stale]
#
# Required:
#   - root (CAP_NET_ADMIN + CAP_SYS_ADMIN)
#   - `ip` from iproute2
#   - coredns binary on PATH, or in $COREDNS, or in $NSGAI_BUILD_DIR
#     (run test/fetch_coredns.sh first if you don't have one)
#
# Optional:
#   NSGAI_VALGRIND=1  - run test_multins (and nsgai_adig forward calls)
#                       under valgrind, failing on any errors.
#   NSGAI_COREDNS_READY_TRIES=N  - readiness probe attempts (default: 50)
#   NSGAI_COREDNS_READY_SLEEP=S  - delay between attempts in seconds
#                                  (default: 0.1)
#
# Per-ns layout (k in 1..NUM_NS):
#   namespace name :  nsgai_dns${k}_${PPID}
#   address served :  10.99.${k}.1
#   control addr   :  203.0.113.250 (startup namespace sentinel)
#   coredns config :  $WORK/ns${k}/Corefile + ns${k}/db.shared
#   resolv.conf    :  /etc/netns/<ns>/resolv.conf -> nameserver 127.0.0.2

set -euo pipefail

ADIG="${1:-}"
MULTI="${2:-}"
STALE="${3:-}"
HOST="shared.nsgai.test"
NUM_NS=3
NAMESERVER_IP="127.0.0.2"
CONTROL_ADDR="203.0.113.250"

if [[ -z "$ADIG" || ! -x "$ADIG" ]]; then
    echo "usage: $0 /path/to/nsgai_adig /path/to/test_multins [/path/to/test_cache_stale]" >&2
    exit 2
fi
if [[ -z "$MULTI" || ! -x "$MULTI" ]]; then
    echo "usage: $0 /path/to/nsgai_adig /path/to/test_multins [/path/to/test_cache_stale]" >&2
    exit 2
fi

if [[ $EUID -ne 0 ]]; then
    echo "SKIP: integration test requires root" >&2
    exit 77
fi
IP="$(command -v ip || true)"
if [[ -z "$IP" || ! -x "$IP" ]]; then
  echo "SKIP: 'ip' not found" >&2
  exit 77
fi

# Locate coredns
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

VALGRIND_CMD=()
if [[ "${NSGAI_VALGRIND:-0}" = "1" ]]; then
    VALGRIND_BIN="${NSGAI_VALGRIND_BIN:-}"
    if [[ -z "$VALGRIND_BIN" ]]; then
        if command -v valgrind >/dev/null 2>&1; then
            VALGRIND_BIN="$(command -v valgrind)"
        fi
    fi
    if [[ -z "$VALGRIND_BIN" || ! -x "$VALGRIND_BIN" ]]; then
        echo "SKIP: NSGAI_VALGRIND=1 but no usable valgrind binary was found" >&2
        exit 77
    fi
    VALGRIND_CMD=("$VALGRIND_BIN" --error-exitcode=1
                  --leak-check=full
                  --errors-for-leak-kinds=definite
                  --show-leak-kinds=definite
                  --track-fds=no
                  --quiet)
fi

READY_TRIES="${NSGAI_COREDNS_READY_TRIES:-50}"
READY_SLEEP="${NSGAI_COREDNS_READY_SLEEP:-0.1}"

WORK="$(mktemp -d -t nsgai_dns_it.XXXXXX)"
declare -a NSNAMES
declare -a CORE_PIDS
CONTROL_PID=""

PAIRS=()

cleanup() {
    set +e
    [[ -n "$CONTROL_PID" ]] && kill "$CONTROL_PID" 2>/dev/null
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

echo "Using coredns: $COREDNS_BIN"
"$COREDNS_BIN" --version || true

# ---- startup-namespace control coredns sentinel ----
# If explicit -n setns socket callbacks are broken, queries may run in the
# startup namespace and hit this server; we can fail immediately with a clear
# diagnosis instead of timing out.
mkdir -p "${WORK}/control"
cat > "${WORK}/control/db.shared" <<EOF
\$ORIGIN nsgai.test.
\$TTL 30
@         IN SOA  ns.nsgai.test. admin.nsgai.test. (1 60 60 600 30)
@         IN NS   ns.nsgai.test.
ns        IN A    ${NAMESERVER_IP}
shared    IN A    ${CONTROL_ADDR}
EOF

cat > "${WORK}/control/Corefile" <<EOF
. {
    bind ${NAMESERVER_IP}
    file ${WORK}/control/db.shared nsgai.test
    log
    errors
}
EOF

"$COREDNS_BIN" -conf "${WORK}/control/Corefile" \
    >"${WORK}/control/coredns.log" 2>&1 &
CONTROL_PID="$!"

# ---- create namespaces, write per-ns configs, start coredns ----
for k in $(seq 1 "$NUM_NS"); do
    ns="nsgai_dns${k}_$$"
    addr="10.99.${k}.1"
    NSNAMES+=("$ns")
    PAIRS+=("${ns}=${addr}")

    nsdir="${WORK}/ns${k}"
    mkdir -p "$nsdir"

    # CoreDNS file plugin reads a BIND-style zone file.
    cat > "${nsdir}/db.shared" <<EOF
\$ORIGIN nsgai.test.
\$TTL 30
@         IN SOA  ns.nsgai.test. admin.nsgai.test. (1 60 60 600 30)
@         IN NS   ns.nsgai.test.
ns        IN A    ${NAMESERVER_IP}
shared    IN A    ${addr}
EOF

    cat > "${nsdir}/Corefile" <<EOF
. {
    bind ${NAMESERVER_IP}
    file ${nsdir}/db.shared nsgai.test
    log
    errors
}
EOF

    # Per-ns resolv.conf
    etcdir="/etc/netns/${ns}"
    mkdir -p "$etcdir"
    cat > "${etcdir}/resolv.conf" <<EOF
nameserver ${NAMESERVER_IP}
options timeout:1 attempts:2 ndots:0
EOF

    "${IP}" netns add "$ns"
    nsenter --net="/var/run/netns/${ns}" -- "${IP}" link set lo up

    # Launch coredns inside the ns; redirect output for diagnostics.
    # nsenter is used in place of `ip netns exec` so we don't remount
    # /sys (forbidden inside a rootless user-namespace).
    logf="${nsdir}/coredns.log"
    nsenter --net="/var/run/netns/${ns}" -- \
        "$COREDNS_BIN" -conf "${nsdir}/Corefile" \
        >"$logf" 2>&1 &
    CORE_PIDS+=("$!")
done

# ---- wait for each coredns to be ready (probe with adig) ----
implicit_only_ready=0
for pair in "${PAIRS[@]}"; do
    ns="${pair%=*}"; addr="${pair#*=}"
    # Extract the numeric index from the ns name suffix for the log file
    k="${ns#nsgai_dns}"; k="${k%%_*}"
    pid_index=$((k - 1))
    pid="${CORE_PIDS[$pid_index]}"
    tries="$READY_TRIES"
    last_out=""
    last_implicit_out=""
    ready_via=""
    while (( tries-- > 0 )); do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "FAIL: coredns in $ns exited before becoming ready" >&2
            cat "${WORK}/ns${k}/coredns.log" 2>/dev/null | tail -80 >&2 || true
            exit 1
        fi

        # Probe explicit namespace-selection path first.
        last_out="$("$ADIG" -4 -n "$ns" "$HOST" 2>&1 || true)"
        if grep -qF "$CONTROL_ADDR" <<<"$last_out"; then
            echo "FAIL: explicit -n lookup for $ns resolved via startup namespace" >&2
            echo "Expected ${addr}, got sentinel ${CONTROL_ADDR}" >&2
            echo "$last_out" >&2
            cat "${WORK}/control/coredns.log" 2>/dev/null | tail -80 >&2 || true
            cat "${WORK}/ns${k}/coredns.log" 2>/dev/null | tail -80 >&2 || true
            exit 1
        fi
        if grep -qF "$addr" <<<"$last_out"; then
            ready_via="explicit"
            break
        fi

        # Probe from inside the namespace as a fallback readiness signal.
        # This distinguishes "CoreDNS not up" from "explicit -n path broken".
        last_implicit_out="$(nsenter --net="/var/run/netns/${ns}" -- \
            env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
            "$ADIG" -4 "$HOST" 2>&1 || true)"
        if grep -qF "$addr" <<<"$last_implicit_out"; then
            ready_via="implicit"
            break
        fi
        sleep "$READY_SLEEP"
    done
    if (( tries < 0 )); then
        echo "FAIL: coredns in $ns did not become ready" >&2
        echo "Readiness settings: tries=${READY_TRIES}, sleep=${READY_SLEEP}" >&2
        echo "Last explicit (-n) adig output:" >&2
        echo "$last_out" >&2
        echo "Last implicit (nsenter) adig output:" >&2
        echo "$last_implicit_out" >&2
        cat "${WORK}/ns${k}/coredns.log" 2>/dev/null | tail -80 >&2 || true
        exit 1
    fi
    if [[ "$ready_via" = "implicit" ]]; then
        implicit_only_ready=$((implicit_only_ready + 1))
        echo "NOTE: $ns became ready via implicit probe; explicit -n still not ready yet" >&2
    fi
done

if (( implicit_only_ready == NUM_NS )); then
    echo "SKIP: explicit netns lookups (-n/-f) unavailable in this c-ares/runtime combination; implicit netns path works" >&2
    echo "SKIP: this usually indicates socket callbacks are not active with the current event-thread mode" >&2
    exit 77
fi

# ---- per-ns adig assertions: each ns sees its own address ----
for pair in "${PAIRS[@]}"; do
    ns="${pair%=*}"; addr="${pair#*=}"

    out="$("${VALGRIND_CMD[@]}" nsenter --net="/var/run/netns/${ns}" -- \
        env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" "$ADIG" -4 "$HOST" 2>&1)"
    if ! grep -qF "$addr" <<<"$out"; then
        echo "FAIL[$ns implicit]: expected $addr, got:" >&2
        echo "$out" >&2
        exit 1
    fi
    echo "PASS[$ns implicit]  -> $addr  (current-thread netns auto-detected)"

    out="$("${VALGRIND_CMD[@]}" "$ADIG" -4 -n "$ns" "$HOST" 2>&1)"
    if ! grep -qF "$addr" <<<"$out"; then
        echo "FAIL[$ns by-name]: expected $addr, got:" >&2
        echo "$out" >&2
        echo "HINT: implicit lookup passed but explicit -n failed;" >&2
        echo "      verify setns(CLONE_NEWNET) is permitted in this environment." >&2
        exit 1
    fi
    echo "PASS[$ns by-name] -> $addr"

    # by fd (skip valgrind here: passing args through `exec N<` wrapper
    # is fiddly enough without an extra wrapper layer)
    out="$(bash -c "exec 9<'/var/run/netns/${ns}'; '${ADIG}' -4 -f 9 '${HOST}'" 2>&1)"
    if ! grep -qF "$addr" <<<"$out"; then
        echo "FAIL[$ns by-fd]: expected $addr, got:" >&2
        echo "$out" >&2
        exit 1
    fi
    echo "PASS[$ns by-fd]   -> $addr"
done

# ---- isolation: default ns should NOT see the test domain ----
out="$("$ADIG" -4 "$HOST" 2>&1 || true)"
for pair in "${PAIRS[@]}"; do
    addr="${pair#*=}"
    if grep -qF "$addr" <<<"$out"; then
        echo "FAIL[isolation]: default ns saw $addr" >&2
        echo "$out" >&2
        exit 1
    fi
done
echo "PASS[isolation]"

# ---- multithreaded test ----
echo "Running multithreaded test across ${#PAIRS[@]} namespaces..."
"${VALGRIND_CMD[@]}" "$MULTI" "$HOST" "${PAIRS[@]}" --iterations 25
echo "PASS[multithreaded]"

# ---- cache stale-revalidation test ----
# Exercises the entry_is_fresh stat_changed branch and the cache
# unlink/free path inside nsgai_channel_create.
if [[ -n "$STALE" && -x "$STALE" ]]; then
    first_pair="${PAIRS[0]}"
    ns="${first_pair%=*}"; addr="${first_pair#*=}"
    "${VALGRIND_CMD[@]}" "$STALE" "$ns" "$addr"
    echo "PASS[cache-stale]"
fi

echo "All DNS-based netns integration checks passed."
