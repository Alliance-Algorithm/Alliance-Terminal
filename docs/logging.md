# 日志规范

本文约定 `teleop-node` / `teleop-client` 的运行日志格式。`ipc-probe` 的 `--stats` 是诊断输出，不属于正式运行日志；它的启动/错误行仍按本规范。

## 1. 行格式

```text
[时间][类别][模块]详情
```

示例：

```text
[0001][INFO][UDS] xxxx
```

- **时间**：进程启动后的秒数，`0000` 起，固定 4 位补零；超过 `9999` 自然加宽；
- **类别**：`DEBUG` / `INFO` / `WARN` / `ERROR`；
- **模块**：大写短名，见下表；
- **详情**：单行、`key=value` 优先，不换行。

一行只表达一个事件，不输出多行块、JSON、完整 protobuf 或 hex dump。

## 2. 模块名

| 模块 | 范围 |
|---|---|
| `NODE` | 进程启动/退出、配置加载、致命错误 |
| `UDS` | control socket 的 connect/accept/handshake/active/disconnect |
| `IPC` | 上行/下行速率、seq 缺口、丢弃、健康状态 |
| `MQTT` | broker 连接/重连、publish 失败、订阅状态 |
| `REC` | 录制开始/停止、文件切换、写盘错误 |
| `UI` | 前端连接/断开、uilink 错误 |
| `VIDEO` | 图传收包/重组异常、解码器状态 |
| `CFG` | 配置来源、热加载、非法配置 |

模块名不随意新增；需要新增时先更新本文。

## 3. 类别语义

| 类别 | 用途 |
|---|---|
| `DEBUG` | 仅调试开关打开时输出；生产默认关闭 |
| `INFO` | 正常状态变化和周期性摘要 |
| `WARN` | 可恢复错误、重试、降级 |
| `ERROR` | 致命错误、配置错误、无法继续 |

`ERROR` 之后进程通常会退出或进入 FATAL；如果只是重连失败，用 `WARN`。

## 4. 该记什么

只记录**必要状态**：

- 启动：版本、配置来源、socket 路径、关键速率；
- 连接：connecting / connected / active / disconnected / reconnecting；
- 重连：原因、下一次重试间隔、累计尝试次数；
- 健康：`rmcs_alive`、`peer_uid`、协议版本；
- 周期性摘要：默认每 10 s 一条，包含 up/down 速率、drop、gap、队列深度；
- MQTT：连接、重连、publish 失败计数；
- 录制：会话开始/结束、文件切换、写盘失败；
- 致命错误：错误码、原因、相关 peer/本地版本。

示例：

```text
[0000][INFO][NODE] start version=0.1.0 socket=/run/teleop/rmcs/control.sock
[0001][INFO][UDS] connected peer_uid=1000
[0001][INFO][UDS] active peer_uid=1000 protocol=2027-V1.0.0
[0010][INFO][IPC] state=active alive=1 rx_up=750 tx_down=500 drop=0 gap=0
[0011][WARN][UDS] disconnected reason=peer_closed retry_ms=100
[0012][INFO][UDS] reconnected attempts=1
[0020][ERROR][NODE] fatal reason=protocol_version peer=2 local=1
```

## 5. 不该记什么

- 每个 `UpData` / `DownData` 帧；
- 每帧的 `seq`、`t_src_ns`、payload 长度（周期摘要里可给速率/计数）；
- payload 内容、protobuf 字段、hex dump；
- 热路径函数入口/出口；
- 每个 UI 鼠标事件、每个视频分片；
- 密码、token、完整配置文件。

热路径只累加计数，由 `INFO` 周期摘要统一输出。

## 6. 线程与输出

- 生产实现使用 **spdlog async**；日志线程不阻塞 IPC/视频热路径；
- 输出到 stderr，由 journald 收集：
  ```bash
  journalctl --user -u teleop-node -f
  ```
- 日志级别热加载只改级别，不改格式；
- 单条日志不拼接多行异常栈；异常只在 `ERROR` 行给原因，必要时另开调试日志。

## 7. `ipc-probe` 的例外

`ipc-probe` 默认每秒输出简洁状态行，`rx_up`/`tx_down` 是**上一秒的包数量**，不是 Hz；`--stats` 保留为兼容写法：

```text
[0001][INFO][IPC] state=active alive=1 rx_up=75 tx_down=50 drop=0 gap=0 peer_uid=1000
```

逐项计数只在 `probe stop` 汇总行给出；需要更细的诊断时再临时加调试输出。

`ipc-probe` 的启动、FATAL、退出摘要仍按本规范：

```text
[0000][INFO][IPC] probe start socket=...
[0001][ERROR][IPC] fatal reason=peer_uid ...
[0010][INFO][IPC] probe stop state=active up_total=750 down_total=500 drop=0 gap=0 ...
```

## 8. 实现约定

- 时间基准用 `CLOCK_MONOTONIC`，从进程启动时记录起点；
- 模块名用编译期常量，避免运行时拼接；
- 日志调用使用结构化参数，不手工拼接大字符串；
- 单元测试至少覆盖格式化函数（时间补零、级别、模块、单行）。
