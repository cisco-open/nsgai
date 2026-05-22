#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Fetch a pinned CoreDNS release binary into the build directory, for use
# by integration_dns_netns.sh.  Run once after configuring the project.
#
#   ./test/fetch_coredns.sh [dest_dir]
#
# Honors COREDNS_VERSION env (default below) and COREDNS_ARCH (auto).

set -euo pipefail

COREDNS_VERSION="${COREDNS_VERSION:-1.11.3}"
DEST="${1:-build}"

case "$(uname -m)" in
    x86_64)  arch="amd64" ;;
    aarch64) arch="arm64" ;;
    armv7l)  arch="arm" ;;
    *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac
COREDNS_ARCH="${COREDNS_ARCH:-$arch}"

mkdir -p "$DEST"
out="$DEST/coredns"

if [[ -x "$out" ]]; then
    echo "coredns already present at $out"
    "$out" --version || true
    exit 0
fi

url="https://github.com/coredns/coredns/releases/download/v${COREDNS_VERSION}/coredns_${COREDNS_VERSION}_linux_${COREDNS_ARCH}.tgz"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Fetching $url ..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$tmp/coredns.tgz"
elif command -v wget >/dev/null 2>&1; then
    wget -q "$url" -O "$tmp/coredns.tgz"
else
    echo "Need curl or wget" >&2
    exit 1
fi

tar -xzf "$tmp/coredns.tgz" -C "$tmp"
mv "$tmp/coredns" "$out"
chmod +x "$out"
echo "Installed $out"
"$out" --version
