# Linux build environment for scripts/build-linux-docker.sh.
#
# Mirrors the CI Linux job: Ubuntu 24.04 (ubuntu-latest), the same apt
# packages, and Rust for the m68k-rs engine (the CI runners ship it).
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build build-essential libsdl1.2-compat-dev libpcap-dev \
      curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
ENV PATH=/root/.cargo/bin:$PATH

WORKDIR /work
