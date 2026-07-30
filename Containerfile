FROM debian:trixie-slim AS build-base

ENV DEBIAN_FRONTEND=noninteractive
ENV CMAKE_BUILD_PARALLEL_LEVEL=2
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    clang-18 \
    cmake \
    curl \
    git \
    libboost-dev \
    libclang-18-dev \
    libcurl4-openssl-dev \
    libedit-dev \
    libpq-dev \
    libssl-dev \
    libzstd-dev \
    lld-18 \
    llvm-18-dev \
    ninja-build \
    python3 \
    zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src

FROM build-base AS styio-build
ARG STYIO_REPO=https://github.com/eBioRing/styio.git
ARG STYIO_REF=nightly
RUN git clone --depth 1 --branch "$STYIO_REF" "$STYIO_REPO" styio
RUN cmake -S styio -B /tmp/styio-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  && cmake --build /tmp/styio-build --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" \
  && mkdir -p /opt/styio/bin \
  && found="$(find /tmp/styio-build -type f -name styio -perm /111 | head -n 1)" \
  && test -n "$found" \
  && cp "$found" /opt/styio/bin/styio

FROM build-base AS pafio-build
ARG PAFIO_REPO=https://github.com/SymPolicy/Pafio.git
ARG PAFIO_REF=nightly
RUN git clone --depth 1 --branch "$PAFIO_REF" "$PAFIO_REPO" pafio
RUN cmake -S pafio -B /tmp/pafio-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  && cmake --build /tmp/pafio-build --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" \
  && mkdir -p /opt/pafio/bin \
  && found="$(find /tmp/pafio-build -type f -name pafio -perm /111 | head -n 1)" \
  && test -n "$found" \
  && cp "$found" /opt/pafio/bin/pafio

FROM build-base AS platform-build
COPY . /src/styio-platform
RUN cmake -S /src/styio-platform -B /tmp/platform-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSTYIO_PLATFORM_BUILD_TESTS=OFF \
  && cmake --build /tmp/platform-build --target styio-platformd \
    --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" \
  && mkdir -p /opt/styio-platform/bin \
  && cp /tmp/platform-build/bin/styio-platformd /opt/styio-platform/bin/styio-platformd

FROM debian:trixie-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    openssl \
    libpq5 \
    python3 \
  && rm -rf /var/lib/apt/lists/*

COPY --from=platform-build /opt/styio-platform/bin/styio-platformd /usr/local/bin/styio-platformd
COPY --from=pafio-build /opt/pafio/bin/pafio /usr/local/bin/pafio
COPY --from=styio-build /opt/styio/bin/styio /usr/local/bin/styio
COPY scripts /opt/styio-platform/scripts
COPY src/PlatformCloud/PackageRegistry /opt/styio-platform/src/PlatformCloud/PackageRegistry
COPY src/PlatformCloud/DeveloperWorkspace/workspace_compile_stress /opt/styio-platform/src/PlatformCloud/DeveloperWorkspace/workspace_compile_stress

ENV PATH="/usr/local/bin:${PATH}"
ENV PYTHONPATH="/opt/styio-platform/src/PlatformCloud/PackageRegistry:/opt/styio-platform/src/PlatformCloud/DeveloperWorkspace"
ENV STYIO_PLATFORM_WORKER_PAFIO_BIN=/usr/local/bin/pafio
ENV STYIO_PLATFORM_WORKER_STYIO_BIN=/usr/local/bin/styio

EXPOSE 8787 8080
ENTRYPOINT ["/usr/local/bin/styio-platformd"]
CMD ["--serve"]
