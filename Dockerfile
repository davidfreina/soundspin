FROM --platform=linux/amd64 ubuntu:22.04

RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        wget gnupg ca-certificates lsb-release && \
    wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc | \
        gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main" \
        > /etc/apt/sources.list.d/kitware.list && \
    apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        gcc-arm-linux-gnueabihf \
        g++-arm-linux-gnueabihf \
        cmake \
        ninja-build \
        git \
        patch && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /work