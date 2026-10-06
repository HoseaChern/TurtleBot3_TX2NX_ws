FROM nvcr.io/nvidia/l4t-base:r32.7.1

RUN apt-get update && apt-get install -y --no-install-recommends \
    make g++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
COPY docker/cuda_test/deviceQuery.cu .
