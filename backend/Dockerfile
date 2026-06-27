# ============================================================
#  TitanX — Multi-Stage C++ Dockerfile
# ============================================================
#
# Stage 1 (builder): ubuntu + gcc + cmake + vcpkg
#   Installs all dependencies from vcpkg.json, then compiles
#   the titanx_server and titanx_simulation binaries.
#
# Stage 2 (runtime): minimal ubuntu
#   Copies only the compiled binaries and required shared libs.
#   Statically linked binaries reduce the final image size.
#
# Build with:
#   docker build -t titanx-engine:latest .
# Run with:
#   docker run -p 8081:8081 \
#     -e TITANX_DB_URL="host=postgres port=5432 dbname=titanx user=titanx password=titanx" \
#     -e TITANX_REDIS_HOST="redis" \
#     titanx-engine:latest

# ---- Stage 1: Builder ----
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build tools
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl \
    zip \
    unzip \
    pkg-config \
    libssl-dev \
    libpq-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg
WORKDIR /opt
RUN git clone https://github.com/microsoft/vcpkg.git && \
    /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"

# Copy project and install dependencies (layer cached unless vcpkg.json changes)
WORKDIR /titanx
COPY vcpkg.json ./
RUN vcpkg install \
    crow \
    nlohmann-json \
    spdlog \
    hiredis \
    jwt-cpp \
    prometheus-cpp \
    gtest \
    --triplet x64-linux

# Copy all source code
COPY . .

# Build
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux \
    -G Ninja && \
    cmake --build build --config Release --parallel $(nproc)

# Run tests during build
RUN cd build && ctest --output-on-failure

# ---- Stage 2: Runtime ----
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Only runtime dependencies (libpq for PostgreSQL, hiredis)
RUN apt-get update && apt-get install -y \
    libpq5 \
    libssl3 \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled binaries from builder
COPY --from=builder /titanx/build/titanx_server      /app/titanx_server
COPY --from=builder /titanx/build/titanx_simulation  /app/titanx_simulation

# Default environment variables
ENV TITANX_HTTP_PORT=8081
ENV TITANX_REDIS_HOST=redis
ENV TITANX_REDIS_PORT=6379
ENV TITANX_DB_URL="host=postgres port=5432 dbname=titanx user=titanx password=titanx"

EXPOSE 8081

ENTRYPOINT ["/app/titanx_server"]
