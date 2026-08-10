# syntax=docker/dockerfile:1@sha256:87999aa3d42bdc6bea60565083ee17e86d1f3339802f543c0d03998580f9cb89

FROM ubuntu@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03 AS build

ARG ATRINIK_PACKAGE_VERSION
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake flex git libcurl4-openssl-dev libidn2-dev \
        libgd-dev libminiupnpc-dev libreadline-dev libssl-dev ninja-build \
        pkgconf python3-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY LICENSE.md ATTRIBUTIONS.md ./
COPY protocol ./protocol
COPY libatrinik ./libatrinik
COPY server ./server
WORKDIR /src/server
RUN python3 tools/dependencies.py sync \
    && python3 tools/dependencies.py verify \
    && cmake -S . -B build/release -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DPACKAGE_TYPE=none \
        -DPACKAGE_VERSION="${ATRINIK_PACKAGE_VERSION}" \
        -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=/src/protocol \
        -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=/src/libatrinik \
    && cmake --build build/release \
        --target atrinik-server plugin_arena plugin_python --parallel

RUN mkdir -p /tmp/worldmaker /tmp/atrinik-data/http /tmp/atrinik-data/tmp \
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
        --httppath=/tmp/atrinik-data/http \
        --resourcespath=/src/server/resources \
    && test -d /tmp/atrinik-data/http/client-maps

FROM ubuntu@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03

ENV DEBIAN_FRONTEND=noninteractive ATRINIK_HTTP_URL=off
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates libcrypt1 libcurl4t64 libgd3 libidn2-0 libminiupnpc21 \
        libpython3.14 libreadline8t64 libssl3t64 python3 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 atrinik

WORKDIR /opt/atrinik
COPY --from=build /src/server/runtime/content/maps ./maps
RUN rm ./maps/python/events/init/documentation.py
COPY --from=build /src/server/runtime/content/lib ./server/lib
COPY --from=build /src/server/resources ./server/resources
COPY --from=build /src/server/install_data ./server/install_data
COPY --from=build /tmp/atrinik-data/http/client-maps ./server/install_data/http/client-maps
COPY --from=build /src/server/atrinikloop.sh ./server/atrinikloop.sh
COPY --from=build /src/server/ca-bundle.crt ./server/ca-bundle.crt
COPY --from=build /src/server/permissions.cfg ./server/permissions.cfg
COPY --from=build /src/server/server.cfg ./server/server.cfg
COPY --from=build /src/server/tools ./server/tools
COPY --from=build /src/server/build/release/atrinik-server ./server/atrinik-server
COPY --from=build /src/server/build/release/libplugin_arena.so ./server/libplugin_arena.so
COPY --from=build /src/server/build/release/libplugin_python.so ./server/libplugin_python.so
COPY --from=build /src/LICENSE.md /usr/share/licenses/atrinik-classic/LICENSE.md
COPY --from=build /src/ATTRIBUTIONS.md /usr/share/doc/atrinik-classic/ATTRIBUTIONS.md
COPY --chmod=0755 server/docker/server-entrypoint.sh /usr/local/bin/atrinik-server-entrypoint
COPY --chmod=0755 server/docker/server-healthcheck.sh /usr/local/bin/atrinik-server-healthcheck

RUN mkdir -p server/data \
    && chown -R atrinik:atrinik /opt/atrinik
USER atrinik
WORKDIR /opt/atrinik/server
EXPOSE 1730/udp
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD ["/usr/local/bin/atrinik-server-healthcheck"]
ENTRYPOINT ["atrinik-server-entrypoint"]
