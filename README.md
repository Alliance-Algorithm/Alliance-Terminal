# Alliance Terminal

RoboMaster 自定义终端的操作端：把 RMCS 的控制数据经 MQTT 送进裁判系统，同时显示机器人状态、接收 HEVC 图传、录制与回放。

> **状态**：架构已封板，代码施工中。进度见 [施工计划](docs/teleop-client-implementation-plan.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [架构与选型](docs/teleop-client-architecture.md) | 三进程、链路、协议、录制、可靠性 |
| [自定义终端协议](docs/custom-terminal-protocol.md) | 官方 32 个 topic 的整理稿与实现要点 |
| [开发容器](docs/teleop-client-dev-environment.md) | 环境基线、容器参数、发布兼容性 |
| [施工计划](docs/teleop-client-implementation-plan.md) | 批次划分、目录、验收标准 |

## 快速开始

当前处于 **B0 骨架**，只能配置：

```bash
cmake --preset dev
```

后续批次会补上 `build`、`ctest` 与 `scripts/sim-demo.sh`。

## 目录

```
proto/      官方协议的转录实现（Apache-2.0）
libs/       可复用库：rm_proto、teleop_ipc（Apache-2.0）
src/        通信进程与前端（GPL-3.0-or-later）
tools/      模拟器：teleop_sim、referee_sim（Apache-2.0）
tests/      单元 / 集成 / fuzz
docker/     开发容器
docs/       设计文档
```

## 许可证

| 目录 | 许可证 |
|---|---|
| `src/` | GPL-3.0-or-later |
| `proto/` `libs/` `tools/` `docker/` | Apache-2.0 |
| `docs/` | CC-BY-4.0 |

应用闭源保护，协议开放互通。SPDX 归属见 `REUSE.toml`。

## English

Alliance Terminal is the operator-side client for the RoboMaster custom terminal protocol. It forwards control data from RMCS over MQTT, renders robot telemetry and the H.265 video feed, and records/replays sessions.

Licensing is split: `src/` is GPL-3.0-or-later; `proto/`, `libs/` and `tools/` are Apache-2.0; `docs/` is CC-BY-4.0.
