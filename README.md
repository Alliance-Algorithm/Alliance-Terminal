# Alliance Terminal

RoboMaster 自定义终端的操作端：把 RMCS 的控制数据经 MQTT 送进裁判系统，同时接收 HEVC 图传、显示机器人状态、录制与回放。

## 架构

系统由三个进程构成，客户端只占其中两个：

| 进程 | 归属 | 职责 |
|---|---|---|
| **RMCS**（含 AT-Bridge） | 独立项目 | 控制律、安全、急停；把控制数据经 UDS 交给客户端 |
| **通信进程** `teleop-node` | 本项目 | 唯一接触 RMCS 与 MQTT 的进程；转发、录制、健康上报 |
| **前端进程** `teleop-client` | 本项目 | 视频解码显示、面板、视频录制、回放 |

三条设计原则：

- **客户端是桥**：不解释业务语义、不做安全判断、不改写 payload 字节。
- **单槽**：状态类数据只保留最新一份，拥塞时覆盖而不是排队。
- **隔离**：前端崩溃不影响控制转发与数据录制；通信进程崩溃由 systemd 拉起。

## 通信结构

| 链路 | 两端 | 传输 | 内容 |
|---|---|---|---|
| **控制链路** | RMCS ↔ 通信进程 | UDS `SOCK_SEQPACKET`，32 字节帧头 | 上行 `CustomControl`（75 Hz，≤60 B）；下行 `CustomByteBlock`（50 Hz，≤300 B） |
| **MQTT** | 通信进程 ↔ 裁判系统服务器 | MQTT 3.1.1，单连接，clientID = 机器人编号 | 9 个上行 / 23 个下行 topic；裸 protobuf；最高 QoS 1 |
| **展示链路** | 通信进程 ↔ 前端 | UDS `SOCK_SEQPACKET`，同一套帧头 | 23 路下行数据 + 健康度；前端回发 7 条指令与录制控制 |
| **图传** | 服务器 → 前端 | UDP **3334** | 8 字节大端头 + 1392 B 分片；HEVC 1080p60；无重传 |
| **录制** | 通信进程 + 前端 → 会话目录 | 文件 | `mqtt_up.pb.zst`、`mqtt_down.pb.zst`、`video/seg_*.mkv` |

Topic 固定 32 个（上行 1–9、下行 10–32），名字与官方协议一致，**不允许自造 topic**。

- 上行：`CustomControl` 走 QoS0 单槽；7 条触发性指令走 QoS1 有界队列。
- 下行：定频状态 QoS0 单槽；触发事件 QoS1 小缓冲。
- 图传包头：帧号 2 B + 分片序号 2 B + 整帧字节数 4 B（大端），后接 ≤1392 B 载荷。

字段级定义见[协议整理稿](docs/custom-terminal-protocol.md)，链路细节与决策见[架构文档](docs/teleop-client-architecture.md)。

## 编译

Ubuntu 24.04 依赖：

```bash
sudo apt install cmake ninja-build g++ protobuf-compiler libprotobuf-dev libgtest-dev
```

```bash
cmake --preset dev        # Debug
cmake --build --preset dev
ctest --preset dev

cmake --preset release    # 性能与延迟结论只认 Release
cmake --preset ci         # Release + -Werror
```

给编辑器（clangd / cpptools）一个编译数据库入口：

```bash
ln -sfn build/dev/compile_commands.json compile_commands.json
```

## 运行

通信进程以用户级 systemd 服务运行，前端是普通桌面程序：

```bash
systemctl --user start teleop-node
journalctl --user -u teleop-node -f

# 前台调试
./build/dev/src/node/teleop-node --config ~/.config/teleop/node.toml

# 前端
./build/dev/src/app/teleop-client
```

| 项 | 路径 |
|---|---|
| 通信进程配置 | `~/.config/teleop/node.toml` |
| 前端配置 | `~/.config/teleop/ui.toml` |
| systemd unit | `packaging/systemd/teleop-node.service` |
| socket | `~/.local/state/teleop/run/`（`rmcs/control.sock`、`ui.sock`） |
| 录制会话 | `~/.local/share/teleop/sessions/<时间戳>/` |

## 目录

```
proto/      官方协议的转录实现（Apache-2.0）
libs/       可复用头文件库：rm_proto、teleop_ipc（Apache-2.0）
src/        通信进程与前端（GPL-3.0-or-later）
tools/      模拟器：teleop_sim、referee_sim（Apache-2.0）
tests/      单元 / 集成 / fuzz
docker/     开发容器
docs/       设计文档
```

## 文档

| 文档 | 内容 |
|---|---|
| [架构与选型](docs/teleop-client-architecture.md) | 三进程、链路、协议、录制、可靠性 |
| [自定义终端协议](docs/custom-terminal-protocol.md) | 官方 32 个 topic 的整理稿与实现要点 |
| [开发容器](docs/teleop-client-dev-environment.md) | 环境基线、容器参数、发布兼容性 |
| [施工计划](docs/teleop-client-implementation-plan.md) | 批次划分、目录、验收标准与进度追踪 |
| [AI 助手约定](Agent.md) | 提交格式、header-only、注释规则、代码与架构约束 |

## 许可证

| 目录 | 许可证 |
|---|---|
| `src/` | GPL-3.0-or-later |
| `proto/` `libs/` `tools/` `docker/` | Apache-2.0 |
| `docs/` | CC-BY-4.0 |

应用闭源保护，协议开放互通。SPDX 归属见 `REUSE.toml`。

## English

Alliance Terminal is the operator-side client for the RoboMaster custom terminal protocol.

It runs as two processes: `teleop-node` owns the only connection to RMCS (UDS) and to the referee system (MQTT 3.1.1, 32 fixed topics, raw protobuf), while `teleop-client` decodes the HEVC video feed (UDP 3334), renders telemetry, and handles recording and replay. The node is a dumb pipe: it never parses or rewrites payloads.

Build with `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` on Ubuntu 24.04.

Licensing is split: `src/` is GPL-3.0-or-later; `proto/`, `libs/` and `tools/` are Apache-2.0; `docs/` is CC-BY-4.0.
