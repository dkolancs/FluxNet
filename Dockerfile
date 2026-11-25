FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config liburing-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFLUXNET_BUILD_TESTS=OFF -DFLUXNET_BUILD_BENCHMARKS=ON -DFLUXNET_BUILD_EXAMPLES=ON \
    && cmake --build build --target fluxnet_echo fluxnet_bench

EXPOSE 9000
CMD ["./build/examples/fluxnet_echo", "--host", "0.0.0.0", "--port", "9000", "--workers", "2"]
