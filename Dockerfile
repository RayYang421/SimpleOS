
FROM ubuntu:24.04


ENV DEBIAN_FRONTEND=noninteractive


RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        qemu-system-arm \
        cpio \
        dosfstools \
        python3 \
        curl \
        make \
        fdisk \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work


CMD ["/bin/bash"]
