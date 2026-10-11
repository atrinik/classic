FROM ubuntu:24.04@sha256:f610ab94648195aa356059f5b41d6085c9d4d903c072430cdd1af7bdb646106b AS builder
COPY tools/ca-certificates.deb /tmp/ca-certificates.deb
RUN echo "641de77d8f142cfd62a1a6f964ba67b20754d3337c480efb529d086075a06c9a  /tmp/ca-certificates.deb" | sha256sum -c - && dpkg-deb -x /tmp/ca-certificates.deb /tmp/ca-bootstrap && mkdir -p /etc/ssl/certs && cat /tmp/ca-bootstrap/usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt && rm -rf /tmp/ca-bootstrap /tmp/ca-certificates.deb
# Ubuntu's signed archive keys authenticate this immutable snapshot. Do not disable signatures.
RUN rm -f /etc/apt/sources.list.d/ubuntu.sources && printf '%s\n' \
    'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] http://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble main universe' \
    'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] http://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble-updates main universe' \
    'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] http://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble-security main universe' > /etc/apt/sources.list && \
    apt-get -o APT::Update::Error-Mode=any update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 perl ca-certificates patchelf file squashfs-tools \
    libxml2-dev libidn2-dev libfreetype-dev libharfbuzz-dev libpng-dev libjpeg-dev \
    libopus-dev libopusfile-dev zlib1g-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
    libxi-dev libxfixes-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev \
    libasound2-dev libpulse-dev libdbus-1-dev libudev-dev libdecor-0-dev libdrm-dev \
    libgbm-dev libegl-dev libgl-dev && dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' > /build-packages.tsv && rm -rf /var/lib/apt/lists/*
COPY sources /inputs/sources
COPY tools /inputs/tools
COPY dependency-inputs.lock.json /inputs/packaging.lock.json
COPY build-dependencies.sh /inputs/
RUN bash /inputs/build-dependencies.sh && rm -rf /inputs/sources /build
COPY packaging.lock.json /opt/atrinik-appimage/packaging.lock.json
ENV PATH="/opt/atrinik-appimage/bin:/opt/atrinik-appimage/tools:${PATH}" \
    CMAKE_PREFIX_PATH=/opt/atrinik-appimage \
    PKG_CONFIG_PATH=/opt/atrinik-appimage/lib/pkgconfig \
    LD_LIBRARY_PATH=/opt/atrinik-appimage/lib
WORKDIR /work

# Minimal test hosts have no application, SDL or graphics/audio development packages.
FROM ubuntu:24.04@sha256:f610ab94648195aa356059f5b41d6085c9d4d903c072430cdd1af7bdb646106b AS smoke-ubuntu24
COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN rm -f /etc/apt/sources.list.d/ubuntu.sources && printf '%s\n' \
 'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble main' \
 'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261010T000000Z noble-updates main' > /etc/apt/sources.list && \
 apt-get -o APT::Update::Error-Mode=any update && apt-get install -y --no-install-recommends binutils util-linux coreutils ca-certificates && rm -rf /var/lib/apt/lists/*

FROM ubuntu:26.04@sha256:88a381d5b5eeb2b35d3ad70925a362c37ce569daf43ede89ff818ec20e4d3794 AS smoke-ubuntu26
COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN rm -f /etc/apt/sources.list.d/ubuntu.sources && printf '%s\n' \
 'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261001T000000Z resolute main' \
 'deb [signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/20261001T000000Z resolute-updates main' > /etc/apt/sources.list && \
 apt-get -o APT::Update::Error-Mode=any update && apt-get install -y --no-install-recommends binutils util-linux coreutils ca-certificates && rm -rf /var/lib/apt/lists/*

FROM debian:13-slim@sha256:918311b7b6c4c6f68b232ba516584925f6c78ad82b6fd534b98979df6438e483 AS smoke-debian13
COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN rm -f /etc/apt/sources.list.d/debian.sources && printf '%s\n' \
 'deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg check-valid-until=no] https://snapshot.debian.org/archive/debian/20261001T000000Z/ trixie main' > /etc/apt/sources.list && \
 apt-get -o APT::Update::Error-Mode=any update && apt-get install -y --no-install-recommends binutils util-linux coreutils ca-certificates && rm -rf /var/lib/apt/lists/*

FROM builder AS build
