#!/usr/bin/env bash
# 构建镜像：./docker/build.sh [dev|ci|base]
#
# 可用环境变量：
#   BASE_IMAGE   基础镜像（默认 ubuntu:24.04）
#   TELEOP_IMAGE 产物镜像名
#   OMZ_REPOS    oh-my-zsh 克隆源，空格分隔多个（默认 官方 → gitee 镜像）
#   BUILD_NETWORK 构建网络模式，默认 host（这样构建里的 git 能用宿主的代理）
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-dev}
base_image=${BASE_IMAGE:-ubuntu:24.04}
image=${TELEOP_IMAGE:-teleop-${target}:24.04}
build_network=${BUILD_NETWORK:-host}

build_args=(--build-arg "BASE_IMAGE=${base_image}")
if [ -n "${OMZ_REPOS:-}" ]; then
    build_args+=(--build-arg "OMZ_REPOS=${OMZ_REPOS}")
fi

docker build \
    "${build_args[@]}" \
    --network "${build_network}" \
    --target "${target}" \
    --tag "${image}" \
    --file "${root}/docker/Dockerfile" \
    "${root}"

printf '\nbuilt %s (base %s)\n' "${image}" "${base_image}"
