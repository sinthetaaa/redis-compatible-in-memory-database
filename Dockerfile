# Base index resolved from Docker Hub on 2026-10-26; supports Linux amd64/arm64.
FROM debian:bookworm-slim@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF -DCMAKE_CXX_STANDARD=20 \
    && cmake --build /build --target redisdb-server --parallel 2 \
    && strip /build/redisdb-server

FROM debian:bookworm-slim@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 redisdb \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin redisdb \
    && mkdir -p /data/snapshots && chown -R 10001:10001 /data
COPY --from=build /build/redisdb-server /usr/local/bin/redisdb-server
ARG VCS_REF=unknown
LABEL io.redisdb.build.configuration="Release" \
      io.redisdb.build.cxx_standard="20" \
      org.opencontainers.image.source="https://github.com/sinthetaaa/redis-compatible-in-memory-database" \
      org.opencontainers.image.revision=$VCS_REF \
      org.opencontainers.image.description="Release C++20 redisdb; explicit container bind; no authentication"
USER 10001:10001
WORKDIR /data
VOLUME ["/data"]
EXPOSE 6380
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/redisdb-server"]
CMD ["--resp2-server", "--bind", "0.0.0.0", "--port", "6380", "--maxmemory", "67108864", "--maxmemory-policy", "noeviction", "--appendonly", "yes", "--aof-file", "/data/appendonly.aof", "--appendfsync", "always", "--snapshot-dir", "/data/snapshots"]
