# This file dockerizes the compilation step. The whole evaluation pipeline cannot be dockerized
# as it directly interacts with hardware (MSP430, Otii Ace Pro, Saleae Logic 2).
#
#   docker build -t bao .
#   docker run --rm -it -v /path/to/gurobi.lic:/licenses/gurobi.lic bao
#
# The base image (Dockerfile.base) provides LLVM and msp430-gcc. Gurobi's license
# forbids redistribution, so it is downloaded here, at build time.

FROM --platform=linux/amd64 ghcr.io/byeongjee/bao-base@sha256:2610de1ea538b7ba842da7417a0f8913ed883401b9e8ba57cf7c4422f7061f5e
ARG GUROBI_VERSION=13.0.0

RUN mkdir -p /opt/gurobi \
    && curl -fsSL "https://packages.gurobi.com/${GUROBI_VERSION%.*}/gurobi${GUROBI_VERSION}_linux64.tar.gz" \
        | tar -xz -C /opt/gurobi --strip-components=1

ENV GUROBI_HOME=/opt/gurobi/linux64 \
    LD_LIBRARY_PATH=/opt/gurobi/linux64/lib \
    GRB_LICENSE_FILE=/licenses/gurobi.lic

WORKDIR /artifact
COPY . .

RUN uv sync --frozen --extra test \
    && cmake -S passes -B passes/build -G Ninja -DLLVM_DIR=/opt/llvm/lib/cmake/llvm \
    && cmake --build passes/build

ENV PATH=/artifact/.venv/bin:$PATH
CMD ["bash"]
