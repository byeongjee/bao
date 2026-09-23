# This file dockerizes the compilation step. The whole evaluation pipeline cannot be dockerized
# as it directly interacts with hardware (MSP430, Otii Ace Pro, Saleae Logic 2).
#
#   docker build -t sawfish .
#   docker run --rm -it -v /path/to/gurobi.lic:/licenses/gurobi.lic sawfish
#
# msp430-gcc is distributed only as an x86-64 Linux binary, so the image is amd64.

ARG LLVM_REPO=https://github.com/byeongjee/llvm-project.git
ARG LLVM_COMMIT=8aa57e39c0f3e62e08558880db2a86eb48e3af85

FROM --platform=linux/amd64 ubuntu:24.04 AS llvm
ARG LLVM_REPO
ARG LLVM_COMMIT
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build python3 g++ \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git init llvm-project && cd llvm-project \
    && git remote add origin "$LLVM_REPO" \
    && git fetch --depth 1 origin "$LLVM_COMMIT" \
    && git checkout FETCH_HEAD
RUN cmake -S llvm-project/llvm -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/llvm \
        -DLLVM_ENABLE_PROJECTS=clang \
        -DLLVM_TARGETS_TO_BUILD="X86;MSP430" \
        -DLLVM_ENABLE_ASSERTIONS=ON \
        -DLLVM_ENABLE_PLUGINS=ON \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_INCLUDE_DOCS=OFF \
    && cmake --build build \
    && cmake --install build

FROM --platform=linux/amd64 ubuntu:24.04
ARG GUROBI_VERSION=13.0.0
ARG MSP430GCC_VERSION=9.3.1.11
ARG MSP430GCC_SUPPORT_VERSION=1.212
ARG TI_URL=https://software-dl.ti.com/msp430/msp430_public_sw/mcu/msp430/MSPGCC/9_3_1_2/export

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl bzip2 unzip git cmake ninja-build g++ make \
    && rm -rf /var/lib/apt/lists/*

COPY --from=llvm /opt/llvm /opt/llvm

RUN mkdir -p /opt/ti \
    && curl -fsSL "$TI_URL/msp430-gcc-${MSP430GCC_VERSION}_linux64.tar.bz2" | tar -xj -C /opt/ti \
    && mv "/opt/ti/msp430-gcc-${MSP430GCC_VERSION}_linux64" /opt/ti/msp430-gcc \
    && curl -fsSL -o /tmp/support.zip "$TI_URL/msp430-gcc-support-files-${MSP430GCC_SUPPORT_VERSION}.zip" \
    && unzip -q /tmp/support.zip -d /tmp \
    && cp -r /tmp/msp430-gcc-support-files/include/. /opt/ti/msp430-gcc/include/ \
    && rm -rf /tmp/support.zip /tmp/msp430-gcc-support-files

RUN mkdir -p /opt/gurobi \
    && curl -fsSL "https://packages.gurobi.com/${GUROBI_VERSION%.*}/gurobi${GUROBI_VERSION}_linux64.tar.gz" \
        | tar -xz -C /opt/gurobi --strip-components=1

COPY --from=ghcr.io/astral-sh/uv:0.12.18 /uv /uvx /usr/local/bin/

ENV LLVM_DIR=/opt/llvm \
    MSP430GCC_TOOLCHAIN_PATH=/opt/ti/msp430-gcc \
    GUROBI_HOME=/opt/gurobi/linux64 \
    LD_LIBRARY_PATH=/opt/gurobi/linux64/lib \
    GRB_LICENSE_FILE=/licenses/gurobi.lic \
    UV_PYTHON_INSTALL_DIR=/opt/python \
    UV_LINK_MODE=copy \
    PATH=/opt/llvm/bin:/opt/ti/msp430-gcc/bin:$PATH

WORKDIR /artifact
COPY . .

RUN uv sync --frozen --extra test \
    && cmake -S passes -B passes/build -G Ninja -DLLVM_DIR=/opt/llvm/lib/cmake/llvm \
    && cmake --build passes/build

ENV PATH=/artifact/.venv/bin:$PATH
CMD ["bash"]
