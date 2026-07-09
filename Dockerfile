# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Test image for nsgai. Provides a full toolchain plus everything the
# netns integration tests need (iproute2, valgrind, lcov/gcovr, a pinned
# CoreDNS binary, c-ares headers/library). Designed to be run via
# scripts/run-container-tests.sh under rootless podman with --privileged,
# which gives the container CAP_NET_ADMIN inside its own user namespace
# so `ip netns add` works without affecting the host.
#
# Build:
#   podman build -t nsgai-test:latest -f Dockerfile .
#
# Run (see scripts/run-container-tests.sh for the canonical invocation):
#   podman run --rm --privileged -v "$PWD":/src:Z -w /src nsgai-test:latest \
#       /src/scripts/container-entrypoint.sh

FROM fedora:44

# Build + test dependencies. install_weak_deps off to keep image small.
RUN dnf install -y --setopt=install_weak_deps=False \
        gcc \
        gcc-c++ \
        make \
        cmake \
        autoconf \
        automake \
        libtool \
        pkgconf-pkg-config \
        glibc-devel \
        iproute \
        iputils \
        valgrind \
        gcovr \
        clang-tools-extra \
        libcmocka-devel \
        git \
        curl \
        ca-certificates \
        tar \
        gzip \
        findutils \
        which \
    && dnf clean all

# Build c-ares from upstream main. The nsgai project tracks c-ares main
# (see project notes); distro c-ares is too old (older nameinfo callback
# signature) and would fail to compile nsgai. Pin via build arg so the
# image is reproducible; bump CARES_REF as needed.
ARG CARES_REPO=https://github.com/c-ares/c-ares.git
ARG CARES_REF=main
ENV CARES_PREFIX=/opt/c-ares
RUN set -eux; \
    git clone --depth 50 --branch "${CARES_REF}" "${CARES_REPO}" /tmp/c-ares; \
    cmake -S /tmp/c-ares -B /tmp/c-ares/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${CARES_PREFIX}" \
        -DCARES_STATIC=OFF \
        -DCARES_SHARED=ON \
        -DCARES_BUILD_TESTS=OFF \
        -DCARES_BUILD_TOOLS=OFF; \
    cmake --build /tmp/c-ares/build -j"$(nproc)"; \
    cmake --install /tmp/c-ares/build; \
    rm -rf /tmp/c-ares; \
    echo "${CARES_PREFIX}/lib64" > /etc/ld.so.conf.d/c-ares.conf; \
    echo "${CARES_PREFIX}/lib"   >> /etc/ld.so.conf.d/c-ares.conf; \
    ldconfig
ENV CMAKE_PREFIX_PATH=${CARES_PREFIX}

# Pre-fetch CoreDNS at image build time so the netns DNS tests never
# need network at runtime. Version pinned to match test/fetch_coredns.sh.
ARG COREDNS_VERSION=1.11.3
RUN set -eux; \
    arch="$(uname -m)"; \
    case "$arch" in \
        x86_64)  ca="amd64" ;; \
        aarch64) ca="arm64" ;; \
        armv7l)  ca="arm"   ;; \
        *) echo "unsupported arch: $arch" >&2; exit 1 ;; \
    esac; \
    url="https://github.com/coredns/coredns/releases/download/v${COREDNS_VERSION}/coredns_${COREDNS_VERSION}_linux_${ca}.tgz"; \
    curl -fsSL "$url" -o /tmp/coredns.tgz; \
    tar -xzf /tmp/coredns.tgz -C /usr/local/bin coredns; \
    rm -f /tmp/coredns.tgz; \
    chmod +x /usr/local/bin/coredns; \
    /usr/local/bin/coredns --version

WORKDIR /src
