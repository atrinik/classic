# Debian testing supplies the SDL 3.4 and SDL3_mixer APIs used by Classic.
# Freeze build and install inputs together; runtime dependencies remain ranges.
FROM debian:forky-slim@sha256:3c80e257eff6274d593eb33a94bda5174ccd619836e6a728a73e11c97f2213e3 AS runtime

RUN rm -f /etc/apt/sources.list.d/debian.sources \
    && printf '%s\n' \
       'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20261009T000000Z/ forky main' \
       > /etc/apt/sources.list \
    && apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update \
    && apt-get install -y --no-install-recommends ca-certificates util-linux \
    && sed -i 's|http://snapshot.debian.org|https://snapshot.debian.org|g' /etc/apt/sources.list \
    && rm -rf /var/lib/apt/lists/*

# The slim image excludes documentation by default. Retain the client notices
# so the clean-install smoke can inspect the installed package's license files.
RUN printf '%s\n' 'path-include=/usr/share/doc/atrinik' \
    'path-include=/usr/share/doc/atrinik/*' > /etc/dpkg/dpkg.cfg.d/zz-atrinik-notices

FROM runtime AS build
RUN apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update \
    && apt-get install -y --no-install-recommends \
       binutils build-essential cmake dpkg-dev file libcurl4-openssl-dev \
       libidn2-dev libsdl3-dev libsdl3-image-dev libsdl3-mixer-dev \
       libsdl3-ttf-dev libssl-dev libvulkan1 libxml2-dev ninja-build \
       pkgconf python3 zlib1g-dev \
    && dpkg-query -W -f='${binary:Package}\t${Version}\n' > /build-packages.tsv \
    && rm -rf /var/lib/apt/lists/*

# Git records the exact source identity; ShellCheck validates packaging scripts.
RUN apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update \
    && apt-get install -y --no-install-recommends git shellcheck \
    && dpkg-query -W -f='${binary:Package}\t${Version}\n' > /build-packages.tsv \
    && rm -rf /var/lib/apt/lists/*
