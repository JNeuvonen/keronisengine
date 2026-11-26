FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl ca-certificates pkg-config \
    zip unzip tar autoconf autoconf-archive automake libtool bison flex \
    python3 python3-venv python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Python virtualenv for quickstart sample-data generation (pyarrow is not an Ubuntu 24.04 apt package)
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir pyarrow
ENV PYTHON=/opt/venv/bin/python

# Rust toolchain for the Hyperliquid order signer crate
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
ENV PATH="/root/.cargo/bin:${PATH}"

# Full vcpkg clone so vcpkg.json's pinned builtin-baseline commit resolves
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src
COPY . .

# Fetch ONNX Runtime and build the engine + tests
RUN ORT=$(./scripts/fetch_onnxruntime.sh) \
    && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DONNXRUNTIME_ROOT="$ORT" \
    && cmake --build build -j"$(nproc)"

ENV PATH="/src/build/bin:${PATH}"
CMD ["trading_engine_tests"]
