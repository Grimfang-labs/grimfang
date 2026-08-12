# Grimfang test-pod image.
#
# Contains the TOOLCHAIN, never the engine and never credentials.
#
# Why pin the base image: the bench signature is a function of the compiler.
# A fixed GCC means a bench mismatch on a pod always signals a real problem,
# rather than "different host, different compiler". Grimfang has already
# shipped UB that MSVC tolerated and GCC optimised away.
#
# All mutable state lives in /workspace so the image stays stateless and a
# host-provided volume can be mounted there.

FROM ubuntu:24.04

LABEL org.opencontainers.image.source="https://github.com/grimfang-labs/grimfang"
LABEL org.opencontainers.image.description="Grimfang test pod: pinned toolchain, fastchess, podctl"

ENV DEBIAN_FRONTEND=noninteractive \
    DATA_DIR=/workspace \
    GRIMFANG_DIR=/workspace/grimfang \
    TZ=UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git tmux rsync curl wget \
        ca-certificates python3 python3-pip bc procps btop htop \
        vim less openssh-server \
    && rm -rf /var/lib/apt/lists/*

# Record the pinned toolchain so any future bench discrepancy is traceable.
RUN { gcc --version | head -1; cmake --version | head -1; ninja --version | sed 's/^/ninja /'; } \
      > /etc/grimfang-toolchain

# fastchess is tooling, not code under test. Prebuild and PIN it -- a
# tournament-manager version bump between pods could change results without
# any engine change.
ARG FASTCHESS_REF=main
RUN git clone https://github.com/Disservin/fastchess.git /opt/fastchess \
    && cd /opt/fastchess && git checkout "${FASTCHESS_REF}" \
    && git rev-parse HEAD > /etc/grimfang-fastchess-ref \
    && make -j"$(nproc)" && strip fastchess || true

RUN mkdir -p /var/run/sshd /root/.ssh /workspace && chmod 700 /root/.ssh

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh \
    && printf '#!/usr/bin/env bash\nexec bash "${GRIMFANG_DIR:-/workspace/grimfang}/tools/podctl.sh" "$@"\n' \
       > /usr/local/bin/podctl && chmod +x /usr/local/bin/podctl

VOLUME ["/workspace"]
WORKDIR /workspace
EXPOSE 22

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["sleep", "infinity"]
