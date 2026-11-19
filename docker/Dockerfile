#
# 构建/测试用镜像。三个阶段：
#   base  编译与测试所需的全部依赖
#   dev   日常开发（加调试器、格式化工具、本地 MQTT broker）
#   ci    CI 用（只多一个 broker，供集成测试）
#
# 目标基线：Ubuntu 24.04（protobuf 3.21 / GCC 13）。

ARG BASE_IMAGE=ubuntu:24.04

FROM ${BASE_IMAGE} AS base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        git \
        libgtest-dev \
        libprotobuf-dev \
        ninja-build \
        pkg-config \
        protobuf-compiler \
        zstd \
    && rm -rf /var/lib/apt/lists/*

ENV CCACHE_DIR=/ccache

# 任何 uid 都能写（容器里跑的用户是宿主 uid，不一定是镜像里的用户）
RUN mkdir -p /ccache && chmod 1777 /ccache

WORKDIR /src

FROM base AS dev

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-format \
        clangd \
        gdb \
        mosquitto \
        mosquitto-clients \
        zsh \
    && rm -rf /var/lib/apt/lists/*

# oh-my-zsh 装给容器里的默认用户 ubuntu（uid 1000，与宿主 uid 一致）。
# 官方仓库优先，连不上就退到国内镜像；可用 --build-arg OMZ_REPOS="url1 url2" 覆盖。
ARG OMZ_REPOS="https://github.com/ohmyzsh/ohmyzsh.git https://gitee.com/mirrors/oh-my-zsh.git"
USER ubuntu
RUN set -e; \
    for repo in ${OMZ_REPOS}; do \
        if HOME=/home/ubuntu timeout 60 git clone --depth 1 "${repo}" /home/ubuntu/.oh-my-zsh; then \
            break; \
        fi; \
        rm -rf /home/ubuntu/.oh-my-zsh; \
    done; \
    if [ ! -d /home/ubuntu/.oh-my-zsh ]; then \
        echo "ERROR: oh-my-zsh 克隆失败，检查网络或用 --build-arg OMZ_REPOS=... 指定镜像" >&2; \
        exit 1; \
    fi; \
    cp /home/ubuntu/.oh-my-zsh/templates/zshrc.zsh-template /home/ubuntu/.zshrc \
    && sed -i 's/^plugins=.*/plugins=(git sudo)/' /home/ubuntu/.zshrc \
    && printf '\nDISABLE_AUTO_UPDATE=true\n' >> /home/ubuntu/.zshrc
USER root

RUN usermod --shell /usr/bin/zsh ubuntu

ENV SHELL=/usr/bin/zsh

FROM base AS ci

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        mosquitto \
        mosquitto-clients \
    && rm -rf /var/lib/apt/lists/*
