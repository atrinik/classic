# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Build with the hash-verified context produced by prepare.py. This independent
# host image deliberately does not copy the application dependency prefix.
FROM ubuntu:24.04@sha256:f610ab94648195aa356059f5b41d6085c9d4d903c072430cdd1af7bdb646106b
COPY tools/ca-certificates.deb /tmp/ca-certificates.deb
RUN echo "641de77d8f142cfd62a1a6f964ba67b20754d3337c480efb529d086075a06c9a  /tmp/ca-certificates.deb" | sha256sum -c - && \
    dpkg-deb -x /tmp/ca-certificates.deb /tmp/ca-bootstrap && \
    mkdir -p /etc/ssl/certs && \
    cat /tmp/ca-bootstrap/usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt && \
    rm -rf /tmp/ca-bootstrap /tmp/ca-certificates.deb
# Signatures remain mandatory; only snapshot expiry is disabled.
RUN rm -f /etc/apt/sources.list.d/ubuntu.sources && printf '%s\n' \
    'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble main universe' \
    'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble-updates main universe' \
    'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble-security main universe' > /etc/apt/sources.list && \
    apt-get -o APT::Update::Error-Mode=any update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates coreutils procps libvulkan1 mesa-vulkan-drivers \
      xvfb x11-utils x11-apps xdotool pulseaudio pulseaudio-utils && \
    dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' > /graphical-host-packages.tsv && \
    rm -rf /var/lib/apt/lists/*
