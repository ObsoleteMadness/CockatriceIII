# Linux-hosted MinGW cross-compilation environment for CockatriceIII's Windows
# builds, used by ../build-local-docker.sh.
#
# Why this exists alongside cross-from-macos.sh: Homebrew's i686-w64-mingw32
# GCC on macOS is built with SJLJ exceptions, but the official Rust
# i686-pc-windows-gnu target's prebuilt std needs DWARF2 unwind symbols
# (_Unwind_Resume etc.), so m68k-rs can't link there. Ubuntu's mingw-w64
# packages build i686 with DWARF2 instead, matching what that std expects
# (this is also what let win32-x86 CI -- which uses MSYS2's own DWARF2 i686
# GCC -- build m68k-rs successfully; see .github/workflows/build-and-release.yml).

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        make \
        curl \
        ca-certificates \
        gcc-mingw-w64-i686 \
        g++-mingw-w64-i686 \
        binutils-mingw-w64-i686 \
        gcc-mingw-w64-x86-64 \
        g++-mingw-w64-x86-64 \
        binutils-mingw-w64-x86-64 \
        mingw-w64-tools \
    && rm -rf /var/lib/apt/lists/*

# Both targets ship win32 (native Windows threads) and posix (pthread)
# alternatives; posix is required for std::thread/std::mutex, which this
# project's CPU-engine glue and Musashi threading both need.
RUN update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-posix \
    && update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix \
    && update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix \
    && update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH="/opt/cargo/bin:${PATH}"

# No --default-host override: let rustup-init detect the actual container
# architecture (this image builds for both amd64 and arm64 hosts, e.g. Apple
# Silicon Docker Desktop/OrbStack -- hardcoding x86_64-unknown-linux-gnu here
# breaks on an arm64 host with "Dynamic loader not found:
# /lib64/ld-linux-x86-64.so.2").
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh \
    && sh /tmp/rustup-init.sh -y --profile minimal \
    && rm -f /tmp/rustup-init.sh \
    && rustup target add i686-pc-windows-gnu x86_64-pc-windows-gnu

COPY build.sh /usr/local/bin/build.sh
RUN chmod +x /usr/local/bin/build.sh

WORKDIR /work/BasiliskII/mingw
ENTRYPOINT ["/usr/local/bin/build.sh"]
