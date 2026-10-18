FROM debian:bookworm-slim AS build
ARG DROGON_REF=v1.9.14

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ca-certificates \
    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev libpq-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "${DROGON_REF}" --recurse-submodules \
    --shallow-submodules https://github.com/drogonframework/drogon.git /tmp/drogon \
    && cmake -S /tmp/drogon -B /tmp/drogon/build \
       -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_INSTALL_PREFIX=/usr/local \
       -DBUILD_EXAMPLES=OFF \
       -DBUILD_CTL=OFF \
       -DBUILD_TESTING=OFF \
       -DBUILD_DOC=OFF \
       -DBUILD_BROTLI=OFF \
       -DBUILD_YAML_CONFIG=OFF \
    && cmake --build /tmp/drogon/build --parallel 2 \
    && cmake --install /tmp/drogon/build

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libjsoncpp25 libuuid1 zlib1g libssl3 libpq5 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home appuser

COPY --from=build /usr/local /usr/local
COPY --from=build /src/build/linear-service /usr/local/bin/linear-service
RUN ldconfig

RUN mkdir -p /app && chown appuser:appuser /app
WORKDIR /app

ENV LD_LIBRARY_PATH=/usr/local/lib
EXPOSE 8080
USER appuser
ENTRYPOINT ["/usr/local/bin/linear-service"]
