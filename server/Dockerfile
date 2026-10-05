# syntax=docker/dockerfile:1@sha256:87999aa3d42bdc6bea60565083ee17e86d1f3339802f543c0d03998580f9cb89

FROM ghcr.io/atrinik/classic-build:1.16.0@sha256:e1c366dbf83ef987765ff913bbb193868314a139ae1e00cc134b22a3159464f2 AS build

# The qualified build image defaults to an unprivileged compiler user.
USER root

ARG ATRINIK_PACKAGE_VERSION
ARG ATRINIK_SOURCE_REVISION=unknown
ARG ATRINIK_SOURCE_DIRTY=unknown
ENV DEBIAN_FRONTEND=noninteractive


WORKDIR /src
COPY LICENSE.md ATTRIBUTIONS.md ./
COPY cmake ./cmake
COPY protocol ./protocol
COPY libatrinik ./libatrinik
COPY server ./server
WORKDIR /src/server
RUN --network=none python3 docker/runtime-provider.py stage \
        --output /opt/runtime-provider \
    && python3 docker/runtime-provider.py packages > /opt/runtime-provider-packages.lock
RUN --network=none python3 tools/dependencies.py sync \
        --cache build/dependencies/downloads --refresh --offline \
    && python3 tools/dependencies.py verify \
    && cmake -S . -B build/release -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DPACKAGE_TYPE=none \
        -DATRINIK_PACKAGE_VERSION="${ATRINIK_PACKAGE_VERSION}" \
        -DATRINIK_SOURCE_REVISION="${ATRINIK_SOURCE_REVISION}" \
        -DATRINIK_SOURCE_DIRTY="${ATRINIK_SOURCE_DIRTY}" \
        -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=/src/protocol \
        -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=/src/libatrinik \
    && cmake --build build/release \
        --target atrinik-server atrinik-access-status plugin_arena plugin_python --parallel

RUN mkdir -p /tmp/worldmaker /tmp/atrinik-assets /tmp/atrinik-data/tmp \
    && cp build/release/atrinik-server \
        build/release/libplugin_arena.so \
        build/release/libplugin_python.so \
        server.cfg permissions.cfg /tmp/worldmaker/ \
    && cp -R install_data/. /tmp/atrinik-data/ \
    && cd /tmp/worldmaker \
    && ./atrinik-server \
        --worldmaker \
        --libpath=/src/server/runtime/content/lib \
        --mapspath=/src/server/runtime/content/maps \
        --datapath=/tmp/atrinik-data \
        --assetspath=/tmp/atrinik-assets \
        --resourcespath=/src/server/resources \
    && test -d /tmp/atrinik-assets/client-maps

FROM ubuntu:26.04@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03

ENV DEBIAN_FRONTEND=noninteractive ATRINIK_HTTP_URL=off
# Public distro trust bootstraps HTTPS without weakening peer/signature checks.
COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
COPY --from=build /opt/runtime-provider-packages.lock /tmp/runtime-provider-packages.lock
COPY --from=build /opt/runtime-provider/ /usr/local/
COPY server/docker/runtime-provider.lock.json server/docker/runtime-provider.py /usr/local/share/atrinik/
# A fixed historical snapshot retains signed Release/package hash validation;
# only the Release expiry timestamp is inapplicable to this immutable archive.
RUN test "$(dpkg --print-architecture)" = amd64 \
    && sed -i \
        -e 's|http://archive.ubuntu.com/ubuntu/|https://snapshot.ubuntu.com/ubuntu/20260810T000000Z/|' \
        -e 's|http://security.ubuntu.com/ubuntu/|https://snapshot.ubuntu.com/ubuntu/20260810T000000Z/|' \
        /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i '/^Signed-By:/a Check-Valid-Until: no' /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update \
    && xargs -r apt-get install -y --no-install-recommends < /tmp/runtime-provider-packages.lock \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-provider-packages.lock \
    && ldconfig \
    && python3 /usr/local/share/atrinik/runtime-provider.py verify-packages \
    && useradd --create-home --uid 10001 atrinik

WORKDIR /opt/atrinik
COPY --from=build /src/server/runtime/content/maps ./maps
RUN rm ./maps/python/events/init/documentation.py
COPY --from=build /src/server/runtime/content/lib ./server/lib
COPY --from=build /src/server/resources ./server/resources
COPY --from=build /src/server/install_data ./server/install_data
COPY --from=build /tmp/atrinik-assets/client-maps ./server/assets/client-maps
COPY --from=build /src/server/atrinikloop.sh ./server/atrinikloop.sh
COPY --from=build /src/server/ca-bundle.crt ./server/ca-bundle.crt
COPY --from=build /src/server/permissions.cfg ./server/permissions.cfg
COPY --from=build /src/server/server.cfg ./server/server.cfg
COPY --from=build /src/server/tools ./server/tools
COPY --from=build /src/server/build/release/atrinik-server ./server/atrinik-server
COPY --from=build /src/server/build/release/atrinik-server-capabilities.json ./server/atrinik-server-capabilities.json
COPY --from=build /src/server/build/release/atrinik-access-status ./server/atrinik-access-status
COPY --from=build /src/server/build/release/libplugin_arena.so ./server/libplugin_arena.so
COPY --from=build /src/server/build/release/libplugin_python.so ./server/libplugin_python.so
COPY --from=build /src/LICENSE.md /usr/share/licenses/atrinik-classic/LICENSE.md
COPY --from=build /src/ATTRIBUTIONS.md /usr/share/doc/atrinik-classic/ATTRIBUTIONS.md
COPY --chmod=0755 server/docker/server-entrypoint.sh /usr/local/bin/atrinik-server-entrypoint
COPY --chmod=0755 server/docker/server-healthcheck.sh /usr/local/bin/atrinik-server-healthcheck

# Keep executable/configuration ancestry root-managed. Runtime persistence and
# generated asset payloads have explicit writable directories. Maps retain their
# existing game ownership for explicit operator map saves.
RUN --network=none python3 /usr/local/share/atrinik/runtime-provider.py verify-runtime \
        --server /opt/atrinik/server \
    && mkdir -p server/data server/assets/data \
    && chown -R atrinik:atrinik maps server/data server/assets/data \
    && chmod 0700 server/data server/assets/data
USER atrinik
WORKDIR /opt/atrinik/server
EXPOSE 1730/udp
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD ["/usr/local/bin/atrinik-server-healthcheck"]
ENTRYPOINT ["atrinik-server-entrypoint"]
