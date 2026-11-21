# syntax=docker/dockerfile:1

# ---- build -----------------------------------------------------------------------------
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
# GCC 14: first libstdc++ with C++20 time zone support (std::chrono::locate_zone).
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-14 cmake ninja-build git curl zip unzip tar pkg-config ca-certificates \
        perl make linux-libc-dev tzdata python3 \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-14 CXX=g++-14 VCPKG_ROOT=/opt/vcpkg VCPKG_DISABLE_METRICS=1

WORKDIR /src

# Dependencies first, in their own layer: rebuilt only when vcpkg.json changes.
COPY vcpkg.json ./
RUN baseline=$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' vcpkg.json) \
    && git clone --filter=blob:none https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout "$baseline" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics \
    && "$VCPKG_ROOT/vcpkg" install --triplet x64-linux --x-install-root=/src/vcpkg_installed

COPY . .
RUN cmake --preset linux-release -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
    && cmake --build --preset linux-release \
    && ctest --preset linux-release \
    && cmake --install out/build/linux-release --prefix /app

# ---- runtime ---------------------------------------------------------------------------
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /app /app

ENV HEATBRAIN_DATA_DIR=/data TZ=Europe/Copenhagen
VOLUME /data
EXPOSE 8080
WORKDIR /app

# `docker exec heatbrain /app/bin/heatbrain plugs` runs any other command in the container.
ENTRYPOINT ["/app/bin/heatbrain"]
CMD ["run"]
