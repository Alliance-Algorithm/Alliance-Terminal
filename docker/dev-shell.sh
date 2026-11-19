#!/usr/bin/env bash
# 进入开发容器（zsh + oh-my-zsh）。
#
# 两条硬性要求：
#   1. 源码必须**同路径**挂载（容器内外同一个绝对路径），否则 compile_commands.json
#      里写的是容器路径，宿主上的编辑器会找不到头文件。
#   2. 容器内 uid 必须等于宿主 uid，构建产物才不会变成 root 所有。
#
# 不设 CPU 配额：cgroup 配额会造成几十毫秒级停顿，对 60 fps 是致命的。
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
image=${TELEOP_IMAGE:-teleop-dev:24.04}
ccache_volume=${TELEOP_CCACHE_VOLUME:-teleop-ccache}

# 镜像里的默认用户是 ubuntu(1000)，oh-my-zsh 装在它的 home 下
if [ "$(id -u)" -eq 1000 ]; then
    container_home=/home/ubuntu
else
    container_home=/tmp
fi

if [ "${1:-}" = "--" ]; then
    shift
fi

if [ "$#" -eq 0 ]; then
    set -- zsh
fi

# -t 只在真正的终端里加；从脚本里调用（无 TTY）时只保留 -i
tty_flag=()
if [ -t 0 ] && [ -t 1 ]; then
    tty_flag=(-t)
fi

exec docker run --rm -i "${tty_flag[@]}" --init \
    --network=host \
    --user "$(id -u):$(id -g)" \
    --env "HOME=${container_home}" \
    --env CCACHE_DIR=/ccache \
    --volume "${root}:${root}" \
    --volume "${ccache_volume}:/ccache" \
    --workdir "${root}" \
    "${image}" "$@"
