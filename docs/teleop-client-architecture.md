# 遥控客户端 · 架构与选型决策文档

| 项 | 内容 |
|---|---|
| 文档版本 | v1.5 |
| 状态 | **架构封板**（第 19 节仅剩外部对接信息，不阻塞实现） |
| 日期 | 2026-11-19 |
| 目标平台 | Linux x86_64 桌面（笔记本 / NUC），X11 与 Wayland |
| 项目性质 | 非商业，开源交流 |
| 变更方式 | 标注 **【定稿】** 的条目为已决策；变更需新增 ADR（`docs/adr/NNNN-*.md`） |

> **v1.2 的三处根本性修正**
> 1. **客户端是"桥"，且在命令回路内**：RMCS → 客户端 → MQTT → 机器人。控制与安全仍全在 RMCS，但客户端不可用时控制会中断（由机器人侧失效策略兜底）。
> 2. **拆成三个进程**：RMCS（独立项目）+ 通信进程 + 前端进程。通信进程最小化依赖，前端进程可崩可关。
> 3. **图传不是 RTP**，是自定义 UDP 协议；**MQTT payload 是裸 protobuf**，不加任何信封。

> **v1.3 的三处修正（环境落地后回填）**
> 1. **Qt 锁定为发行版 6.4**（原写 6.8）：`QQuickRhiItem`（6.7+）不可用，视频面固定走 `QQuickFramebufferObject`。
> 2. **protobuf 改用发行版 3.21**（原写 Conan 固定）：Ubuntu 24.04 已满足对端 ≥3.15 的要求。
> 3. 新增 **§14.7 开发容器**，环境细节见《开发容器设计文档》（`teleop-client-dev-environment.md`）。

> **v1.4 的九处修正（官方协议落地后回填，依据 `custom-terminal-protocol.md`）**
> 1. **方向口径统一**：**上行 = 客户端 → 服务器/机器人**；**下行 = 服务器/机器人 → 客户端**。原稿把上行写成"RMCS 快照"、下行写成"远端命令"，与实际协议方向相反。
> 2. **协议固定为官方 32 个 topic**（上行 9 / 下行 23）：topic 名 = 消息名、payload = 裸 protobuf、**最高 QoS 1**、**clientID = 机器人编号**、**无认证**、**不得自造 topic**——原 §6.5 的 `veh/{id}/status`（LWT + retained）删除。
> 3. **MQTT 只开一条连接**：同一个 clientID 开两条会互相踢下线。
> 4. **RMCS 只碰两条通道**：上行 `CustomControl`（75 Hz 状态快照）、下行 `CustomByteBlock`（50 Hz）。其余 22 条下行只经展示链路送前端。**IPC 通道从 32 缩为 2**。
> 5. **前端可以发指令**：7 条触发性指令（`MapClickCmd` / `AssemblyCommand` / `RobotPerformanceSelectionCommand` / `CommonCommand` / `RuneActivateCommand` / `DartCommand` / `SentryCtrlCommand`）由前端产生，经通信进程白名单转发；**前端仍然无法让 RMCS 收到任何消息**。
> 6. `KeyboardMouseControl`（75 Hz 键鼠）**v1 不实现**，协议与代码里留位；将来实现时该路必须"累加"而不是"覆盖"。
> 7. **录制改为三份**：上行、下行、视频；不再单独录 IPC。
> 8. **3D 场地地图移出 v1**（低优先级，保留模块位置）。
> 9. 图传与官方一致：**UDP 3334**、8 字节大端头、1392 B 分片、60 fps / 1080p / HEVC。

> **v1.5 的三处修正（部署与桌面壳确定后回填）**
> 1. **启动模型定为"用户级"**：通信进程 = `systemd --user` 服务（登录即起、崩溃自动拉起）；前端 = 桌面程序 + 托盘常驻。**不建系统用户、不需要 root。**
> 2. **socket 移到 home 下** `~/.local/state/teleop/run/`：`rmcs/control.sock` 单独一个子目录、只把它 bind mount 进 RMCS 容器；`ui.sock` 留在宿主。不用 `$XDG_RUNTIME_DIR`——容器可能先于登录启动，那时该目录不存在。
> 3. **新增 §4.5 托盘与进程控制**：点 X 隐藏到托盘；右键菜单含 面板/状态/通信进程/设置/退出；**"退出"会一并停止通信进程**；关闭通信进程需二次确认（比赛中额外提示）；托盘不可用时照样隐藏并提示"再启动一次可唤回"。

---

## 0. 摘要

本系统由三个进程构成，客户端只占其中两个：

| 进程 | 归属 | 一句话职责 |
|---|---|---|
| **RMCS**（含 AT-Bridge 模块） | 独立项目 | 控制 + 安全；把内部消息通过 UDS 送出 |
| **通信进程** | 本项目 | 唯一接触 RMCS 与 MQTT 的进程；转发 + 录制 + 健康上报 |
| **前端进程** | 本项目 | 视频、面板、3D 地图、视频录制、回放 |

核心主张：

> **客户端是一条"不思考"的低延迟管道。全链路三处单槽、永不排队、只覆盖不阻塞。因为每一帧都是全局状态快照，丢掉旧帧只会损失时间分辨率，不会丢失信息。**

### 十条不变量

1. **只有通信进程**接触 RMCS 与 MQTT；前端进程两者都不接触。
2. **前端进程无法让 RMCS 收到任何消息**——IPC 里不存在这条路（前端可以直接发 MQTT 指令，那是另一条路，见 §7.5）。
3. 前端进程崩溃或关闭，**控制链路与数据录制不受影响**。
4. 任何一端变慢，**只允许丢弃，不允许阻塞上游**。
5. 数据源失效时**停止发送**，不重发旧状态。
6. 视频**残帧整帧丢弃**，永不喂给解码器。
7. **MQTT payload 零修改**，不加信封。
8. 回放**只读文件**，不产生任何外发消息。
9. 全链路单一 `CLOCK_MONOTONIC` 时基。
10. **所有丢弃可计数、可见、可录制**。

### 关键决策表

| # | 决策 | 理由 |
|---|---|---|
| 1 | C++20 + Qt6/QML + GStreamer | 与 VA-API、dmabuf、Qt 场景图同栈 |
| 2 | 三进程：RMCS / 通信 / 前端 | 把最易崩的部分与控制路径隔离 |
| 3 | 控制链路 UDS `SOCK_SEQPACKET` 单连接 | 75 Hz + 低频命令；内核保证边界与可靠 |
| 4 | 数据语义：全局状态快照 + latest-wins | 幂等、可丢、免 resync |
| 5 | `payload` 由 RMCS 侧生成，客户端零修改 | 桥接层与业务 schema 完全解耦 |
| 6 | MQTT payload = 裸 protobuf，topic = 消息名 | 对端规范；客户端不做信封 |
| 7 | 图传自定义 UDP 协议，自写帧重组器 | 对端不提供 RTP 封装 |
| 8 | 视频录制直接存压缩流，不转码 | CPU≈0，零画质损失 |
| 9 | 录制跨两进程，用同一 session 目录对齐 | 无需跨进程传文件句柄 |
| 10 | 回放 = 前端换源，通信进程照常运行 | 不让 UI 获得停控制链路的能力 |
| 11 | 分层授权：应用 GPL-3.0，协议层 Apache-2.0 | 协议要被 RMCS 项目复用 |

---

## 1. 项目定位与约束

### 1.1 系统上下文

```
                    ┌──────────────────────┐
                    │ 服务器 / 机器人        │  ← 进程外
                    └──────────▲───────────┘
                               │ MQTT
┌─ RMCS 进程 ─────┐   UDS   ┌──┴───────────────┐   UDS   ┌─ 前端进程 ────────┐
│ RMCS 控制 + 安全 │◀───────▶│   通信进程         │◀───────▶│ 视频 / 面板 / 3D   │
│ AT-Bridge       │         │ 转发+录制+健康     │         │ 视频录制 / 回放    │
└─────────────────┘         └──────────────────┘         └───────────────────┘
                                      ▲
                                      │ UDP（图传）
                             ┌────────┴─────────┐
                             │ 图传发送端         │  ← 进程外
                             └──────────────────┘
```

### 1.2 定位

客户端扮演**桥 + 观察窗 + 记录仪**：

- **不解释**：业务语义（字段含义、控制律、安全判据）全在 RMCS 侧；
- **不决策**：不做任何安全判断；
- **不改写**：RMCS 给的 `payload` 原样转发，**一个字节都不改**。

### 1.3 硬约束

| 约束 | 影响 |
|---|---|
| Linux x86_64（笔记本/NUC） | 依赖 VA-API 硬解 |
| X11 与 Wayland 双支持 | 呈现路径必须抽象 |
| 图传：有线、1080p60、HEVC、**无重传** | 丢一个分片丢整帧，恢复只能靠发送端 IDR |
| 图传：自定义 8 字节头 + 1392 字节分片 | 必须自写重组器，不能用 `rtpjitterbuffer` |
| 上行 **`CustomControl` 75 Hz**；7 条指令触发式 ≤10 Hz | 采样量化 13.3 ms，客户端延迟已非瓶颈 |
| **MQTT payload = 裸 protobuf** | 不能加时间戳/序号/设备号/丢弃计数 |
| **32 个固定 topic**（9 上行 / 23 下行），**最高 QoS 1** | topic 由官方协议定义；**不得自造 topic** |
| **clientID = 机器人编号**；无认证 | 一台客户端对应一台机器人 |
| proto 中无时间戳/序号/设备号 | 远端无法判断新旧与丢包；由机器人控制方处理 |
| 机器人失效策略待定义 | 客户端只承诺"可观测的尽力而为" |
| 必须支持录制回放 | 强制统一时基与**三份录制**（上行 / 下行 / 视频） |
| MQTT 3.1.1 基线 | 无 message expiry / user properties |
| 非商业 + 开源交流 | 协议必须开放才能形成生态 |

### 1.4 非目标

- 不做跨平台；不做操控输入；不做安全判断；
- 不做多客户端仲裁、多路图传、音频；
- 不做 3D 动态编辑、实时阴影、后处理；
- 不做云端服务、自动更新、Flatpak/Snap、Web UI；
- **不修改对端 proto**（加时间戳/序号/设备号）。

### 1.5 职责边界【定稿】

| 职责 | RMCS 侧 | 通信进程 | 前端进程 | 远端 |
|---|---|---|---|---|
| 控制律、安全、急停、看门狗 | ✅ 全权 | ❌ | ❌ | ❌ |
| `CustomControl` 快照生成（≤60 B） | ✅ | ❌ 只转发 | ❌ | — |
| `CustomByteBlock` 消费（≤300 B） | ✅ | ❌ 只转发 | ❌ | — |
| MQTT 收发（9 上行 / 23 下行） | — | ✅ | ❌ | ✅ |
| 7 条触发性指令的发起 | — | ❌ 只转发 | ✅ | — |
| 指令的业务效果与去重 | ❌ 不做 | ❌ 不解析 | ❌ | ✅ 服务器 / 机器人 |
| 图传接收、解码、显示 | — | ❌ | ✅ | — |
| 数据录制（上行 + 下行） | — | ✅ | ❌ | — |
| 视频录制 | — | ❌ | ✅ | — |
| 回放 | — | ❌ | ✅ | — |
| 3D 地图 | — | ❌ | ⏳ v1 不做 | — |

### 1.6 边界约束

#### P0：违反即返工

| 编号 | 约束 | 结论 |
|---|---|---|
| **P0-1** | **客户端在命令回路内，但不在安全回路内** | 客户端失效会导致控制中断，这是已知且已接受的；安全判据全在 RMCS 与机器人侧 |
| **P0-2** | **通信进程禁止依赖 UI 框架** | 不得 include `QtQuick`/`QtQml`/`QtWidgets`/`GStreamer`，CI 用 grep 守住 |
| **P0-3** | **转发路径上不得修改 `payload` 字节** | 物理上也无法违反：payload 是不透明 `bytes` |

#### 其余约束

| 编号 | 约束项 | 结论 |
|---|---|---|
| S1 | 图传路数 | 单路 |
| S2 | 音频 | 不做（即使码流含音频轨也丢弃） |
| S3 | 多目标 | 单目标 |
| S4 | 操控输入 | 客户端不产生控制输入 |
| R1 | 运行形态 | 通信进程 = `systemd --user` 服务；前端 = 桌面程序 + 托盘常驻（§4.4 / §4.5） |
| R2 | 单实例 | **两个进程各 `flock` 单实例**（图传 UDP 端口独占 + 录制目录共享，不允许多开） |
| R3 | 启动顺序 | 登录时 systemd --user 拉起通信进程；前端启动时若没在跑会自动拉起 |
| Q1 | 硬件基线 | Intel Gen9+ 硬解；**无硬解也允许启动**，降级软解并显式告警；1080p60 |
| Q2 | 可靠性 | **可观测的尽力而为**（见第 13 节） |
| Q3 | 可观测性 | 桥接健康度常驻显示；丢弃全部计数；一键诊断包 |
| T1 | 本机信任模型 | **两个进程都用当前桌面用户**；不需要 root、不建系统用户；`SO_PEERCRED` 只做兜底 |
| T2 | MQTT 安全 | **无认证**（协议未定义用户名/密码/TLS），只校验 clientID = 机器人编号 |
| T3 | 录制敏感性 | 不加密，README 明确告知 |
| E1 | 协议兼容 | 官方协议随赛季更新；主版本不兼容即拒绝；能力位图代替次版本 |
| E2 | schema 权威 | **官方《自定义终端协议》为唯一真源**；本仓库 `proto/` 是转录实现，`custom-terminal-protocol.md` 是整理稿 |
| E3 | 配置热加载 | 不做（仅日志级别、UI 主题可热改） |
| C1 | 国际化 | 从第一天用 `qsTr()`，中/英 |
| C2 | 文档语言 | README 双语，设计文档中文 |

---

## 2. 数据通路

| 通路 | 方向 | 特征 | 目标 | 失败行为 |
|---|---|---|---|---|
| **上行·控制** | RMCS → 通信进程 → MQTT | `CustomControl` ≤60 B，75 Hz，状态快照 | RMCS 产出 → publish 返回 ≤2 ms (p99) | 丢旧留新，不排队 |
| **上行·指令** | 前端 → 通信进程 → MQTT | 7 条触发性指令，≤10 Hz，QoS1 | ≤100 ms | 不丢（有界缓冲，满则拒绝并告警） |
| **下行·数据** | MQTT → 通信进程 → 前端（`CustomByteBlock` 另抄送 RMCS） | 23 个 topic，多数可丢 | ≤100 ms | 定频丢旧留新；触发式不丢 |
| **展示链路** | 通信进程 → 前端进程 | 24 个通道（23 下行 + 健康） | ≤1 ms | 丢旧留新，绝不影响上游 |
| **图传** | 网络 → 前端进程 | 64.5 KB/帧 @60 fps，无重传 | **glass-to-glass ≤60 ms** | 丢整帧，绝不喂残帧 |

**关键观察**：只有图传是真正的连续流，也只有它是延迟大头；上行只有一条 75 Hz 状态流加若干触发指令，下行全是可覆盖的状态，复杂度极低。

---

## 3. 总体架构

### 3.1 三进程视图

```
┌─ RMCS 进程 ──────┐   UDS    ┌─ 通信进程（headless）────┐   UDS    ┌─ 前端进程（桌面）────────┐
│                  │          │                          │          │                          │
│  控制 + 安全      │◀────────▶│  ipc ─▶ fanout ─┬─▶ mqtt │          │  视频管线 ─┬─▶ 面板/3D   │
│                  │  控制链路 │                 ├─▶ 录制 │◀────────▶│            └─▶ 视频录制  │
│  AT-Bridge       │          │      ui_server ◀┘        │  展示链路 │  面板 ◀─▶ 回放器         │
└──────────────────┘          └───────────┬──────────────┘          └──────────────────────────┘
                                          │ MQTT
                                          ▼
                              服务器 / 机器人（进程外）
```

### 3.2 三条链路 + 一条支路

| 链路 | 两端 | 方向 | 传输 | 说明 |
|---|---|---|---|---|
| **控制链路** | AT-Bridge ↔ 通信进程 | `CustomControl` ↑ / `CustomByteBlock` ↓ | UDS `SEQPACKET` | 唯一接触 RMCS 的地方，**只有 2 个通道** |
| **MQTT 链路** | 通信进程 ↔ 裁判系统服务器 | 上行 9 / 下行 23 | MQTT 3.1.1 **单连接** | 唯一接触外部网络的地方 |
| **展示链路** | 通信进程 ↔ 前端进程 | 下行数据 ↓ / 指令与本地操作 ↑ | UDS `SEQPACKET` | 前端指令只走 MQTT，**绝不**进入控制链路 |
| **图传支路** | 图传发送端 → 前端进程 | 单向 | UDP 3334 | 不经过通信进程 |

### 3.3 分层与依赖（单向）

```
app ──▶ ui / video / record / replay / scene ──▶ bus ──▶ core
bridge: ipc / mqtt / record ──▶ core
```

- 通信进程**不含**任何 UI、GL、解码依赖；
- 前端进程**不含** MQTT、不接触 RMCS；
- 这是 P0-2 的落地方式。

### 3.4 核心抽象

#### 3.4.1 归一化总线

```cpp
enum class SampleKind { Video, Topic, Health };

struct Sample {
    SampleKind kind;
    int64_t    t_mono_ns;
    uint32_t   topic_id;   // 32 个官方 topic 之一
    VideoFrame video;      // kind == Video
    ByteView   payload;    // kind == Topic：原始 protobuf 字节，前端按 topic_id 反序列化
};
```

#### 3.4.2 `DataSource`

```cpp
class DataSource {
public:
    virtual ~DataSource() = default;
    virtual bool poll(Sample& out) = 0;
};
```

- `LiveSource`：前端进程里读展示链路的适配器；
- `ReplaySource`：读 session 目录。

**【定稿】回放不是另一套系统，只是把源换掉。**

#### 3.4.3 `LatestValueRegister`（核心数据结构）

```cpp
template <typename T>
class LatestValueRegister {
public:
    void set(T&& v);          // 覆盖，不排队
    bool try_take(T& out);    // 取走最新
    uint64_t overwritten();   // 被覆盖计数
};
```

全链路三处：**生产者单槽 → 内核小缓冲 → 消费者单槽**。

### 3.5 设计原则

1. 客户端不思考：不改 payload、不做业务判断、不做安全决策。
2. 反压不跨进程边界：只丢弃并留痕，绝不阻塞另一侧。
3. 全链路单槽：队列深度恒为 1。
4. UI 吃快照与合帧，不吃事件流。
5. 零拷贝到显示：收包 → 解码 → dmabuf → GPU 纹理。
6. 时间戳贯穿全链路。
7. **不为已不存在的瓶颈做优化**：75 Hz 下默认关闭实时调度、绑核、`mlockall`。

---

## 4. 进程与线程模型

### 4.1 通信进程内部

```
ipc_client ──▶ CustomControl ──┬──▶ mqtt_tx（单槽）──▶ 裁判系统服务器
     ▲                        ├──▶ data_recorder
     │                        └──▶ fanout ──▶ ui_server ──▶ 前端进程
     └──────── CustomByteBlock 队列 ◀── mqtt_rx ◀─────────┘
```

| 部件 | 职责 |
|---|---|
| `ipc_client` | 连 AT-Bridge，收 `CustomControl`/HEALTH，发 `CustomByteBlock`，维护 session/超时 |
| `mqtt` | **单连接**；上行单槽 + 指令队列；下行按类别分发 |
| `fanout` | 下行按通道分流：前端一份（全部 23 条）+ RMCS 一份（仅 `CustomByteBlock`） |
| `ui_server` | 接受前端连接、推下行数据、收 `UI_CMD`（录制控制 / 白名单指令） |
| `data_recorder` | 写 `mqtt_up.pb.zst` / `mqtt_down.pb.zst` |

### 4.2 前端进程内部

```
视频管线（UDP→重组→解包→硬解）─┬─▶ 面板 / 3D
                                └─▶ 视频录制（压缩流，不转码）
展示链路 client ──▶ TelemetryStore ──▶ 面板
回放器 ◀─ session 目录 ──▶ 总线 ──▶ 面板
```

### 4.3 线程表

| 进程 | 线程 | 调度 | 备注 |
|---|---|---|---|
| 通信 | `ipc_rx` | 默认 | 持续 drain |
| 通信 | `ipc_tx` | 默认 | 有界队列，满则拒绝 |
| 通信 | `mqtt_io` | 默认 | Paho 自管 |
| 通信 | `mqtt_pub` | 默认 | 单槽 |
| 通信 | `recorder` | 默认/低 | 有界 2 s，满则丢录制 |
| 通信 | `ui_server` | 默认 | 每订阅者单槽 |
| 前端 | `udp_rx` | 默认 | `recvmmsg` |
| 前端 | `reassemble` | 默认 | 与 `udp_rx` 同线程 |
| 前端 | `video_dec` | 默认 | GStreamer 自管 |
| 前端 | UI 主线程 | 默认 | 60 Hz，只读快照 |

**默认不做实时化**。`SCHED_FIFO`、绑核、`mlockall` 均为"实测超标再开"。

### 4.4 部署与生命周期【定稿】

**两个进程都跑在当前桌面用户下：不需要 root、不建系统用户、不加组。**

| 项 | 设计 |
|---|---|
| 通信进程 | `systemd --user` 服务 `~/.config/systemd/user/teleop-node.service`，`WantedBy=default.target`、`Restart=always`、`RestartSec=0.2` |
| 前端 | 桌面程序 `~/.local/share/applications/teleop-client.desktop`，可设为登录自启；常驻托盘 |
| socket 目录 | `~/.local/state/teleop/run/`（0700） |
| 控制 socket | `~/.local/state/teleop/run/rmcs/control.sock` —— **只把这个子目录 bind mount 进 RMCS 容器** |
| 展示 socket | `~/.local/state/teleop/run/ui.sock` —— 宿主内部，不挂进容器 |
| 校验 | `SO_PEERCRED` 只做兜底（uid 与当前用户一致） |
| 启动 | 登录 → systemd --user 拉起通信进程 → 连 `control.sock`（RMCS 未就绪则指数退避） |
| 停止 | `systemctl --user stop teleop-node`，优雅退出（见 §4.5） |
| 日志 | `journalctl --user -u teleop-node` |
| 前端未启动 | 通信进程照常转发，**但录制开不了**（数据、视频都不录） |

> **为什么不用 `/run/user/<uid>`**：容器可能先于用户登录启动，那时该目录不存在，Docker 会把 mount 源当成空目录并**以 root 创建**，之后权限全乱。home 下的路径一直都在。

> **容器侧的三个前提**（需要 RMCS 项目配合）：
> 1. 容器以**宿主 uid** 运行（`--user $(id -u):$(id -g)`），否则 0700 目录进不去、socket 属主也不对；
> 2. 容器参数加 `-v $HOME/.local/state/teleop/run/rmcs:$HOME/.local/state/teleop/run/rmcs`，内外路径一致；
> 3. AT-Bridge 启动时先 `unlink` 旧 socket 再 `bind`；通信进程把 `ECONNREFUSED` 当作"对方未就绪"，继续退避重试。

### 4.5 托盘与进程控制【定稿】

前端常驻系统托盘（StatusNotifierItem）。托盘是"找回客户端"的主要入口，但**不是唯一入口**：再启动一次程序会唤起已有窗口。

**右键菜单**

| 菜单项 | 行为 |
|---|---|
| 面板 | 显示 / 前置主窗口 |
| 状态 | 打开健康度小窗（桥接、MQTT、录制、视频） |
| 通信进程 | 子菜单：显示状态；`重启通信进程`；`关闭通信进程…`（二次确认） |
| 设置 | 打开设置对话框 |
| 退出 | **停止通信进程 + 关闭前端**（完全退出） |

**窗口关闭（点 X）**

- 有托盘：**隐藏到托盘，不弹确认**；
- 无托盘：照样隐藏，弹 3 秒提示"已隐藏到后台；再次启动 `teleop-client` 即可唤回"。

**两处二次确认**

| 动作 | 文案 |
|---|---|
| 关闭通信进程 | 常规："关闭后机器人将收不到控制数据，只能靠机器人侧失效保护。" 比赛中（`GameStatus.current_stage == 4`）：标题改为 **"目前正处于比赛中，确定关闭吗？"**，默认按钮为"取消" |
| 退出 | 录制中："退出会结束本次录制。" 比赛中：同样加提示 |

**状态来源**

- 通信进程是否在跑：以 **`ui.sock` 连没连上**为准（真实状态）；`systemctl` 只用来下命令；
- 菜单里的状态项在 `QMenu::aboutToShow` 时刷新；
- 前端启动时若通信进程没跑，自动 `systemctl --user start teleop-node`，等 `ui.sock` 最多 2 秒，失败则明确报错。

**退出顺序**：收尾视频分段 → `systemctl --user stop teleop-node`（等它 flush 录制、断开 MQTT）→ 退出前端进程。

**无 systemd 的机器**（极少）：降级为"前端 fork 子进程"模式，托盘菜单不变，重启由前端负责。

---

---

## 5. 控制链路（AT-Bridge ↔ 通信进程）【定稿】

### 5.1 选型

> **`AF_UNIX` + `SOCK_SEQPACKET`，单条双向连接；AT-Bridge 作为 server（listen），通信进程 connect。**

延迟：单程 **30–100 µs**（75 Hz 下非瓶颈）。

| 备选 | 淘汰理由 |
|---|---|
| 共享内存 | 75 Hz 下收益为零，却要处理崩溃残留与撕裂检测 |
| `SOCK_DGRAM` | 缓冲满静默丢弃，命令不可接受 |
| `SOCK_STREAM` | 需自行分帧，白送 bug |
| TCP loopback | 完整协议栈 + Nagle |
| ZeroMQ / DDS / D-Bus / gRPC | 依赖重、语义复杂，单机双进程杀鸡用牛刀 |

**为什么单连接**：数据（RMCS→客户端）与命令（客户端→RMCS）是相反方向，内核中本就是两份独立缓冲，不存在队头阻塞。

### 5.2 帧格式

```
偏移  字段            类型   说明
0     magic           u16    0x544C ('TL')
2     proto_version   u8     当前 1
3     msg_type        u8     见 5.3
4     flags           u16    保留，未定义位必须忽略
6     body_len        u16    protobuf 长度
8     session_id      u64    每次连接重新生成
16    seq             u64    发送方单调递增
24    t_mono_ns       i64    CLOCK_MONOTONIC
32    body            ...    protobuf
```

`session_id` / `seq` / `t_mono_ns` 以帧头为准，body 内不重复。无 CRC。

### 5.3 消息类型

| 值 | 名称 | 方向 | 频率 | 说明 |
|---|---|---|---|---|
| 1 | `HELLO` | C→S | 连接时 | 必须 |
| 2 | `HELLO_ACK` | S→C | 连接时 | 必须 |
| 3 | `UP_DATA` | S→C | 75 Hz | 携带 `CustomControl`；**可丢**（丢即丢，取最新） |
| 4 | `DOWN_DATA` | C→S | ≤50 Hz | 携带 `CustomByteBlock`；可靠 |
| 6 | `HEALTH` | S→C | 1–2 Hz | 尽力 |
| 100–199 | **预留给对端扩展** | — | — | — |
| 200+ | 实验性 | — | — | 默认忽略 |

> `CMD` / `CMD_ACK` / `LINK_STATUS` 已删除：新协议下客户端不需要向 RMCS 回执，也不需要把 MQTT 连接状态上报给 RMCS。

### 5.4 通道（只有两个）

通道字典不再是"由 RMCS 下发的一整张表"，而是**协议里固定两条**：

| id | 通道 | 方向 | 频率 | payload |
|---|---|---|---|---|
| 1 | `CustomControl` | AT-Bridge → 通信进程 | 75 Hz | `CustomControl.data`（≤60 B，**当前状态快照**） |
| 2 | `CustomByteBlock` | 通信进程 → AT-Bridge | 50 Hz | `CustomByteBlock.data`（≤300 B） |

- 两者都是**不透明字节**，通信进程不解析、不改写；
- 其余 30 个 topic 完全不经过 IPC；
- 官方 32 个 topic 的完整编号/名字/QoS 表放在 `proto/topics.inc`，只用于 MQTT 侧与前端。

> **为什么不再有通用通道字典**：32 个 topic 的映射已由官方协议固定，且只有 2 个需要跨进程；动态字典只会增加握手复杂度。协议若更新，改表 + 重编即可。

### 5.5 协议定义（`proto/ipc.proto`，Apache-2.0）

```protobuf
syntax = "proto3";
package teleop.ipc;

message Hello {
  uint32 proto_version  = 1;
  uint64 capabilities   = 2;
  string client_version = 3;
}

message HelloAck {
  uint32 proto_version = 1;
  uint64 capabilities  = 2;
  string protocol_rev  = 3;   // 官方协议版本，如 "2027-V1.0.0"
}

// —— 上行：RMCS 给客户端的控制快照，原样发到 MQTT ——
message UpData {
  uint32 channel  = 1;   // 固定 1 = CustomControl
  int64  t_src_ns = 2;   // RMCS 产出时刻（同机单调钟）
  bytes  payload  = 3;   // CustomControl 序列化字节，零修改
}

// —— 下行：服务器给机器人的自定义数据块，原样转给 RMCS ——
message DownData {
  uint32 channel   = 1;  // 固定 2 = CustomByteBlock
  int64  t_recv_ns = 2;  // 客户端从 MQTT 收到的时刻
  bytes  payload   = 3;  // CustomByteBlock 序列化字节，零修改
}

message Health { uint32 state = 1; uint32 load_pct = 2; }
```

- `body` 就是上表里的**具体消息本身**，不再套一层 `Envelope`（`msg_type` 已经说明类型了）；
- `session_id` / `seq` / `t_mono_ns` 只在 32 字节帧头里，body 内不重复。

### 5.6 数据语义

- `UpData`：每帧都是**当前控制快照**（绝对值，不是增量），所以丢任意帧不影响正确性，拥塞时丢旧留新；
- `DownData`：来自服务器的 50 Hz 数据块，客户端只负责搬运；丢一帧由 RMCS 侧按缺口处理。

字段用途：

| 字段 | 用途 |
|---|---|
| 帧头 `seq` | 本地丢失检测、记录缺口、录制打标 |
| 帧头 `session_id` | 检测 AT-Bridge 重启 |
| `t_src_ns` | **A2 延迟指标的起点**（RMCS 产出 → publish 返回） |

### 5.7 两条数据的路径

```
RMCS 控制循环（75 Hz）
   → UpData{CustomControl}
   → 通信进程：不解析，按 topic `CustomControl` 发 QoS0
   → 裁判系统服务器 → 图传链路 → 机器人

机器人 → 图传链路 → 服务器
   → 通信进程收到 `CustomByteBlock`（50 Hz，QoS0）
   → DownData（IPC） + UI_DATA（展示链路）
   → AT-Bridge → RMCS 内部接口；前端按需显示
```

**三条明确的责任划分**：

| 事项 | 谁负责 | 为什么 |
|---|---|---|
| 控制律、安全、急停 | **RMCS** | 客户端不做任何决策 |
| 7 条触发性指令的业务效果 | **服务器 / 机器人** | 客户端只负责把字节发出去 |
| 指令的重复与过期 | **服务器 / 机器人** | 客户端不解析 payload，无法去重 |

客户端**不产生** `CMD_ACK`、**不伪造**任何业务回执；服务器侧的回执（如 `SentryCtrlResult`）走普通下行。

### 5.8 反压必须被切断（最重要的工程约束）

```
AT-Bridge ──send──▶ 内核缓冲 ──recv──▶ 通信进程 ──publish──▶ MQTT
           ↑ 客户端慢时 EAGAIN          ↑ MQTT 慢时积压
```

1. **生产者侧单槽**：AT-Bridge 在 `send` 返回 `EAGAIN` 时**丢弃本帧并保留最新值**，绝不阻塞 RMCS 的控制循环；
2. **内核缓冲要小**：`SO_SNDBUF`/`SO_RCVBUF` 设 2–4 帧；
3. **消费者持续 drain**：`ipc_rx` 一直读到 `EAGAIN`，不能因 MQTT 慢而停读；
4. **MQTT 侧单槽**：新帧覆盖旧帧，不排队；
5. **丢弃留痕**：每处覆盖计数，UI 常驻显示，录制打标。

> 结果：从 RMCS 产出到 MQTT 出网，最坏延迟 = 一个内核缓冲深度 + 一次发布，不随运行时长增长。

### 5.9 超时与健康

| 判据 | 阈值 | 动作 |
|---|---|---|
| 无 `UP_DATA` | > 200 ms | UI 黄色告警 |
| 无 `UP_DATA` | > 1 s | UI 红色 + 状态置离线 + 录制打标 |
| `HEALTH` 丢失 | > 3 s | 判定 RMCS 侧失联 |
| 恢复 | — | 自动恢复，记一次事件 |

### 5.10 连接状态机

```
CONNECTING ──connect──▶ HANDSHAKE ──HELLO/HELLO_ACK──▶ ACTIVE
     ▲                      │                              │
     │                  主版本不兼容                 心跳超时 / POLLHUP
     │                      ▼                              ▼
     └──── backoff ────   FATAL                         LOST
```

重连：`100 ms → 200 → 400 → … → 2 s` 封顶，±20% 抖动。重连后清空状态，等下一帧，**不做 resync 请求**。

### 5.11 安全与资源边界

- `SO_PEERCRED` 校验；
- `socket(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`；`accept4` 同理；
- `recvmsg` + `MSG_TRUNC` 检测截断；
- 单帧上限 64 KB，超限断开；
- 热路径无动态扩容；fd 数量固定。

### 5.12 时钟

同机 `CLOCK_MONOTONIC` 系统级一致，`t_src_ns` 可直接做单向延迟测量，无需同步。**绝不用 `CLOCK_REALTIME` 做差值**。

### 5.13 逃生口

仅当上行频率升到 kHz 级、或单帧 > 256 KB、或 IPC 单程 p99 > 1 ms 时，才考虑把上行通道换共享内存。帧头与 schema 不变，上层无感。

---

## 6. MQTT 链路

### 6.1 库与版本

- **Eclipse Paho MQTT C**，`MQTTAsync` + RAII 封装；
- 协议基线 **MQTT 3.1.1**；
- protobuf **≥3.15**：Ubuntu 24.04 自带 **3.21**，直接用发行版包即可（22.04 自带 3.12 不满足，故基线定为 24.04）。

### 6.2 连接拓扑

| 项 | 结论 |
|---|---|
| 连接数 | **一条**（同一个 clientID 开两条会被服务器互相踢下线） |
| clientID | **机器人编号**（1 / 3 / 4 / 7 / 101 / 103 / …） |
| 认证 | **无**（协议未定义用户名/密码/TLS） |
| `clean_session` | **true**（断连期间不排队，避免迟到指令被执行） |
| `TCP_NODELAY` | 必须开 |
| retained / LWT | **不用**（协议只有 32 个 topic，不允许自造在线状态 topic） |

> 原 v1.3 的"数据连接 + 命令连接"双连接设计作废：QoS 改为**按消息类型**在单连接上分别设置。

### 6.3 上行

| topic | 频率 | QoS | 队列 | 说明 |
|---|---|---|---|---|
| `CustomControl` | 75 Hz | 0 | **单槽**（状态快照，覆盖即可） | 丢旧留新 + 计数 |
| 7 条指令（`MapClickCmd` 等） | 触发，≤10 Hz | 1 | **有界 FIFO（32）** | 满则拒绝并告警，**已入队的不丢** |
| `KeyboardMouseControl` | — | — | — | **v1 不实现**；将来实现时该路必须"累加"，不能覆盖 |

- 指令限速在通信进程兜底：每条 ≤10 Hz；`MapClickCmd` 按协议 1.3.1 的 0.5 s / 3 s 间隔。
- **无离线缓冲**：断连时上行直接丢弃，重连后只发最新状态。

**QoS0 不等于非阻塞**：TCP 发送缓冲填满仍会阻塞。用 `MQTTAsync`，监控未完成发送量与写阻塞时长，超阈值告警。

### 6.4 下行

| 类别 | topic | QoS | 缓冲 | 说明 |
|---|---|---|---|---|
| 定频状态 | `CustomByteBlock`(50 Hz)、`RobotDynamicStatus`(10 Hz)、`GameStatus`(5 Hz)、其余 1 Hz | 0 | **每通道单槽** | 丢旧留新，下一秒还有 |
| 触发事件 | `Event`、`PenaltyInfo`、`SentryCtrlResult`、`MapClickInfo`、`RadarInfoToClient`、`RobotPathPlanInfo`、`Buff` | 1 | 小缓冲（256） | **不可丢**，溢出插 `EVENT_GAP` |

### 6.5 Topic 与 payload

- **只有 32 个 topic**（9 上行 / 23 下行），名字与官方协议一字不差、大小写敏感；完整表见 `proto/topics.inc`；
- **payload = protobuf 原始字节，零修改**；
- **不自造 topic**：原 `veh/{id}/status`（LWT + retained）删除；调试信息只留在本地（UI + 录制）。

| 原本想加 | 现在 | 后果 |
|---|---|---|
| `device_id` | ❌ | clientID 已表明身份 |
| `client_seq` | ❌ | 远端无法检测丢包 |
| `t_src_ns` | ❌ | 远端无法判断数据新旧 |
| `dropped_since_last` | ❌ | 远端无法知道缺口 |

这些信息**只保留在本地**：通信进程记录、展示链路传给 UI、录制里打标。远端如何判新由机器人控制方处理（已确认）。

### 6.6 断连与重连

- 断连期间**直接丢弃**，单槽继续被覆盖，计数器累加；
- 重连后立刻取当前最新帧发布，不补发历史；
- 不做 resync（每帧都是全量）。

### 6.7 3.1.1 的补偿

- 无 `message expiry` → 用 `clean_session=true` 从源头避免积压；
- 命令幂等 → 由 RMCS 负责；
- 无 user properties → 不做额外元数据。

---

## 7. 展示链路（通信进程 ↔ 前端进程）【定稿】

### 7.1 选型

`AF_UNIX` + `SOCK_SEQPACKET`；通信进程 listen `~/.local/state/teleop/run/ui.sock`；复用同一套 32 字节帧头。

### 7.2 消息类型

| 类型 | 方向 | 频率 | 可靠性 |
|---|---|---|---|
| `UI_HELLO` / `UI_HELLO_ACK` | 双向 | 连接时 | 必须 |
| `UI_DATA` | B→U | ≤50 Hz | **可丢**（每通道单槽） |
| `UI_HEALTH` | B→U | 1–2 Hz | 尽力 |
| `UI_RECORD_STATE` | B→U | 变化时 | 可靠 |
| `UI_CMD` | U→B | 罕见 | 可靠 + ack |
| `UI_CMD_ACK` | B→U | 随命令 | 必须 |

### 7.3 协议（`proto/ui_link.proto`）

```protobuf
message UiHello {
  uint32 proto_version  = 1;
  string ui_version     = 2;
}

message UiHelloAck {
  uint32 proto_version = 1;
  uint64 capabilities    = 2;
  repeated ChannelDef channels = 3;   // 32 个固定 topic 的编号/名字/QoS
  UiRecordState record  = 4;
  string protocol_rev   = 5;          // 官方协议版本
}

message ChannelDef {
  uint32 id       = 1;   // 1..32
  string topic    = 2;   // 官方 topic 名
  uint32 qos      = 3;   // 0 或 1
  bool   sendable = 4;   // 是否允许前端发（只有 7 条指令为 true）
}

message UiData {
  uint32 channel          = 1;
  uint64 seq              = 2;
  int64  t_recv_ns        = 3;
  uint64 drops_since_last = 4;   // ★ 仅本地可见，不上 MQTT
  bytes  payload          = 5;   // 原样字节
}

message UiHealth {
  bool   ipc_connected    = 1;
  bool   mqtt_connected   = 2;
  uint32 ui_queue_depth   = 3;
  uint64 dropped_total    = 4;
  uint32 mqtt_blocked_ms  = 5;
  uint32 latency_p50_us   = 6;
  uint32 latency_p99_us   = 7;
  uint64 last_data_age_ms = 8;
}

message UiRecordState {
  bool   active      = 1;
  string session_dir = 2;
}

message UiCmd {
  uint32 corr    = 1;
  uint32 op      = 2;   // 1=START_RECORD 2=STOP_RECORD 3=PUBLISH 4=RELOAD_UI
  uint32 channel = 3;   // op=3 时必填，必须是 sendable 通道
  bytes  payload = 4;   // op=3 时为 protobuf 序列化字节
}

message UiCmdAck {
  uint32 corr   = 1;
  uint32 result = 2;   // 0=成功 1=队列满 2=通道不可发 3=MQTT 未连
}
```

### 7.4 单槽与背压

通信进程给**每个订阅者**维护一个单槽：`send` 返回 `EAGAIN` 就丢弃并计数，**绝不停下读 IPC**。

前端侧读法：一直读到 `EAGAIN`，只保留每个 channel 的最后一帧。

> **UI 打开一个慢速调试面板，不能拖慢控制路径。**

### 7.5 前端能发什么

```
前端面板 ──UI_CMD{op=PUBLISH, channel, payload}──▶ 通信进程 ──▶ MQTT
前端面板 ──UI_CMD{START_RECORD|STOP_RECORD}──────▶ 通信进程 ──▶ 录制器
```

- **只允许发 7 条指令**（`sendable = true`），其余通道一律拒绝（`result = 2`）；
- 录制状态由**通信进程**持有；**前端崩溃不停止录制**（只有显式 `STOP_RECORD` 才停）；
- 前端重启后从 `UI_HELLO_ACK.record` 得知当前会话，接着录新分段；
- **`UI_CMD` 永远不会被转发到 IPC 控制链路**——协议里没有这条路。

> **已知取舍**：前端因此必须认识官方协议（自己序列化这 7 条、反序列化 23 条下行）。代价换来的是"桥可以长期不重启、不随 UI 改动而更新"。

### 7.6 宽容接收

| 情况 | 客户端行为 |
|---|---|
| 未知 protobuf 字段 | 忽略，继续工作 |
| 未知 `channel` | 照常转发到 MQTT；UI 显示"未配置通道" |
| 未知消息类型 | 跳过并计数，不断连 |
| 对端不发某类消息 | 正常工作，不等待 |
| 主版本不兼容 | **拒绝连接**并明确告警（唯一 fail-fast 点） |
| 帧超长/字段异常 | 上限保护，超限断开并计数 |

---

## 8. 图传（自定义 UDP / H.265）

### 8.1 协议

| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | `frame_id` | `u16` BE | 帧号递增；`65536/60 ≈ 18.2 分钟`回绕 |
| 2 | `frag_index` | `u16` BE | 帧内分片序号 |
| 4 | `frame_bytes` | `u32` BE | 整帧码流字节数 |
| 8 | payload | ≤ 1392 B | 分片数据 |

- 单个 UDP 包 ≤ 1400 字节；
- 1080p60 HEVC，**无重传、无反馈**；
- 端口固定 **UDP 3334**（仅开发联调时可配）；服务器 `192.168.12.1`，终端 `192.168.12.2`。

由此推导：

```
分片数 n    = ceil(frame_bytes / 1392)
第 i 片范围 = [i*1392, min((i+1)*1392, frame_bytes))
收齐判据    = 已收字节数 == frame_bytes 且无重叠
```

### 8.2 重组器

```
UDP socket（SO_RCVBUF 8 MB）→ recvmmsg 批量收（64 包/次）→ 重组器
```

| 规则 | 处理 |
|---|---|
| 槽位 | 4 个环形复用（2–3 帧在途足够） |
| 单帧上限 | 4 MB（一次性预分配，4 槽 = 16 MB） |
| 重复分片 | 位图去重 |
| 越界分片 | 丢弃并计数 |
| **残帧** | **整帧丢弃，绝不喂解码器** |
| 超时淘汰 | 更新的帧已完成，或 > 50 ms 未收齐 |
| 帧号跳变 | 判定丢帧，计数 |
| 帧号回绕 | 用 `int16` 差值比较 |
| 发送端重启（帧号归零） | "时间间隔 + 帧号大幅回退"启发式判定，清空重组器 |

**为什么绝不喂残帧**：H.265 的 P/B 帧依赖前帧，残缺的参考帧会污染后续所有帧，导致长时间花屏。丢一帧的代价远小于喂一个坏帧。

### 8.3 管线

```
重组器 → appsrc (video/x-h265, stream-format=byte-stream, alignment=au,
                 framerate=60/1, 1920x1080)
        → h265parse
        → tee ─┬─▶ queue(leaky=downstream) → vah265dec → qml6glsink
               └─▶ queue(leaky=downstream) → splitmuxsink（录制，不转码）
```

- `alignment=au`：一个 buffer = 一帧完整 Access Unit；
- 录制分支 `leaky=downstream`：磁盘慢时丢录制，绝不反压解码。

### 8.4 时间戳（协议里没有，得自己造）

| 场景 | 做法 |
|---|---|
| 显示 | `sync=false`，解码完立刻呈现 |
| 录制 | `PTS = Δframe_id × (1s/60)`，锚定首帧到达时刻 → 恒定 60 fps，丢帧表现为空洞 |

### 8.5 丢包恢复

无重传、无反馈，意味着：**丢一个分片 → 丢整帧 → 画面花到下一个 IDR**。

所以 **IDR 间隔直接等于最坏恢复时间**。客户端无法改变，只能：

1. 运行时测出 GOP 并显示给操作员；
2. 丢包后在 UI 上明确标记"正在等待关键帧"，而不是让操作员以为设备卡了。

### 8.6 未知参数 → 运行时测量

对端未提供位深、IDR 间隔、SPS/PPS 策略，因此改为**主动测量**：

| 测什么 | 怎么测 | 用途 |
|---|---|---|
| 分辨率 / 帧率 | `h265parse` caps + 滚动窗口实测 | 与标称 60 对比，掉帧可见 |
| 位深 / profile | caps 或 VPS 解析 | 决定能否硬解，不能就降级并告警 |
| **GOP（IDR 间隔）** | IDR NAL 时间差的中位数 | 直接告诉操作员"丢包后最长恢复约 X 秒" |
| SPS/PPS 是否存在 | 启动 3 秒内是否见到 VPS/SPS | 没有 → 明确报错，提示发送端需开启 repeat-headers |
| 丢包 | 帧号跳变 + 残帧计数 | 画质问题第一现场 |
| 解码耗时 | 每帧打点 | 判断是否临近掉帧 |

统一呈现在"视频健康度"面板，与桥接健康度并排。

### 8.7 解码器

| 硬件 | 元素 | 注意 |
|---|---|---|
| Intel iGPU | `vah265dec`（GStreamer ≥1.20）/ `vaapih265dec` | `LIBVA_DRIVER_NAME=iHD` 或 `i965` |
| NVIDIA | `nvh265dec` | 需 plugins-bad + 专有驱动 |
| 混合显卡 | **强制 iGPU 解码** | 否则隐式回拷，白送 5–15 ms |
| 兜底 | `avdec_h265` | 1080p60 软件解码可能掉帧，必须显式告警 |

上线前：`vainfo | grep -i hevc` 确认 `HEVCMain` / `HEVCMain10`。

### 8.8 呈现（X11 / Wayland）

| 维度 | X11 | Wayland |
|---|---|---|
| vsync | 可关，可撕裂换延迟 | 不能强制立即呈现 |
| 合成 | 可禁用；全屏 unredirect | 永远合成，除非 direct scanout |
| 零拷贝 | EGL + dmabuf | 原生 dmabuf |
| 多窗口叠加 | 容易 | 需 subsurface |

应对：抽象 `VideoOutput`；`QT_QPA_PLATFORM=wayland;xcb`；接受 Wayland 的 +1 帧；**不录屏，只录源码流**。

---

## 9. 录制

### 9.1 跨进程约定（用文件系统，不用 IPC）

```
session_YYYYMMDD_HHMMSS/
  manifest.json          ← 通信进程写（时基、32 topic 表、官方协议版本、会话元信息）
  mqtt_up.pb.zst         ← 通信进程：实际发出的上行消息
  mqtt_down.pb.zst       ← 通信进程：实际收到的下行消息
  video/
    seg_00000.mkv        ← 前端进程：压缩流直存，不转码
    seg_00001.mkv
    segments.jsonl       ← 前端进程：追加写，崩溃安全
```

- **不录 IPC**：上行的 payload 就是 RMCS 给的字节，录"实际发出去的"即可；下行同理——少一份文件，少一处不一致。
- 目录由**通信进程**创建、写 manifest；
- 前端通过 `UI_RECORD_STATE` 得知目录，向同一目录写视频；
- **UI 崩溃只丢视频，数据完整；重开 UI 接着录新分段**。

### 9.2 录制什么

| 内容 | 来源 | 用途 |
|---|---|---|
| 上行消息 | 实际 publish 的 `topic + 字节` | 证明"我们发了什么" |
| 下行消息 | 实际收到的 `topic + 字节` | 证明"我们收到了什么" |
| 视频 | UDP 收流（压缩流直存） | 画面复盘 |
| 健康度 | 桥接指标（写进 manifest 或独立采样文件） | 解释"为什么当时卡" |

> 上行 + 下行 + 视频三者同一时基，争议时才能回答"是 RMCS 没给，还是我们没发出去"。

**记录格式**（`mqtt_up.pb.zst` / `mqtt_down.pb.zst`）：

```
每条记录 = u32 长度 + Record{ int64 t_mono_ns; uint32 topic_id; uint32 direction; bytes payload; }
zstd 每 100 ms 或每 64 条 flush 一次
读取时遇到尾部截断，直接丢弃该条
```

`segments.jsonl` 每段一行：

```json
{"file":"seg_00000.mkv","t_start_ns":0,"t_end_ns":0,"frames":0,"anchor_frame_id":0,"anchor_t_ns":0}
```

`anchor_frame_id` / `anchor_t_ns` 用于"每段开头校准"视频时间轴（见 §10.2）。

### 9.3 格式

**分段 MKV（`splitmuxsink`）**：崩溃只丢最后一段，每段从 IDR 开始，天然形成 seek 索引。录制开始若正好在 GOP 中间，**先等下一个 IDR 再落盘**（最多一个 GOP，约 1–2 s）。

禁止：MP4（moov 在尾，崩溃即全毁）。

### 9.4 时间轴

- 全系统只用 `CLOCK_MONOTONIC`；
- manifest 记录一次 `realtime ↔ monotonic` 偏移；
- 视频 PTS 由帧号推导；数据 PTS 用记录里的 `t_mono_ns`（上行 = RMCS 产出时刻，下行 = 收到时刻）；
- **录制器不重新打时间戳，只透传**。

### 9.5 不得影响实时路径

1. 独立线程 + 有界队列（约 2 s）；
2. 队列满 → 丢录制并计数，绝不反压；
3. 视频不转码；
4. 大块写（≥1 MB）、预分配、`posix_fadvise`；
5. `fsync`：分段关闭时；manifest 每 5–10 s 原子更新。

### 9.6 容量

H.265 1080p60 ≈ 9–22 GB/小时（取决于码率）。必须实现按容量/时长的**滚动保留**。

---

## 10. 回放

### 10.1 架构：前端换源

```
LiveSource  = 展示链路适配器
ReplaySource = 读 session 目录（mkv + sidecar）
        ↓ 同一个 DataSource 接口
      前端总线 → 面板 / 3D / 图表
```

**【定稿】回放只在前端换源，通信进程照常运行。**

理由：通信进程由 systemd 独立托管，前端无权停它；停掉会触发 LWT，让远端误判设备离线，也等于让 UI 获得了"停控制链路"的能力。

### 10.2 四个实现细节

1. **换源时清空状态**：`TelemetryStore`、图表缓冲、轨迹线全部重置；
2. **时基切换**：live 用实时单调钟，replay 用虚拟时钟；UI 必须显式标注 `LIVE` / `REPLAY`；视频时间轴按"帧号 × 1/60"推进，**每段开头用 `anchor_t_ns` 校准一次**，避免长录漂移；
3. **退出回放**：重建 `LiveSource`，收到第一帧前显示"正在恢复实时"，不显示陈旧值；
4. **建议**：回放期间自动暂停两个录制器（录下来的是"操作员在看录像"，无价值）。

### 10.3 安全约束

**回放只读文件，不产生任何外发消息。** 回放期间前端**主动禁用** `UI_CMD{PUBLISH}`，且前端进程本身没有 MQTT 客户端、也没有通往控制链路的消息类型——这是双重保证。

---

## 11. 3D 静态地图

### 11.1 选型

**【v1 不做】3D 场地地图移出 v1**（低优先级，先保留目录与接口位）。原定方案留存备查：Qt Quick 3D + glTF 2.0（`.glb`），三角面 < 500 万。

理由：与面板共用 QML 场景图；`Model{source:"map.glb"}` 直接加载；`View3D.pick()`/`mapFrom3DScene()` 现成；许可干净。

> 若将来超过 500 万面，改用 `cgltf` + 自写 GL 渲染器（约 2–3k LOC）。

### 11.2 资产管线

```
建模工具 → .glb（贴图内嵌）→ gltfpack（量化压缩，降 50–70%）→ assets/map/world.glb
```

- 资产进 **Git LFS**；
- **不要塞进 `.qrc`**（改地图就要重编译），放外部目录按路径加载；
- 资产许可单独放 `assets/map/LICENSE`。

### 11.3 恒定光照

1. **烘焙进 albedo（推荐）**：Cycles 烘 AO/日照，乘进基础色贴图，运行时 unlit；
2. 顶点色；
3. 单方向光 + 环境光（`castsShadow: false`）。

**明确不做**：实时阴影、SSAO、Bloom、PBR IBL。

### 11.4 坐标系

```
WorldFrame: ENU（东-北-天），右手系，米，Z 轴向上
原点：georef_origin = { lat, lon, alt }
```

- glTF 是 Y-up 右手系；
- **只在 `View3D` 根节点放一个 `Node{ eulerRotation.x: -90 }`**，其余全部用 ENU；
- 经纬度转换在 `geo/` 里做，**不在 QML 里做数学**。

### 11.5 性能纪律

面板尺寸渲染；静态相机时降到 30 Hz 或按需重绘；合并网格/`InstancedMesh` 减少 draw call；与视频共享同一 GL 上下文。

---

## 12. 前端层

### 12.1 框架

**【定稿】Qt 6.4（Ubuntu 24.04 发行版）+ QML**，`QSG_RHI_BACKEND=opengl`。

> Qt 6.4 的约束：**没有 `QQuickRhiItem`**（6.7+ 才有），所以视频面只能用 `QQuickFramebufferObject`。这不影响架构，但决定了 §12.4 的实现路径。

### 12.2 数据绑定

- 通信进程/前端 reader 线程**绝不**直接 emit 到 QML；
- 只更新 `TelemetryStore`；
- 60 Hz 定时器读快照，比较 `version` 后 emit；
- 图表降采样到面板像素宽度；
- **桥接健康度与视频健康度常驻显示**，不藏二级页面。

### 12.3 图表

**【定稿】Qt Charts（QML）** + 三条纪律：`replace()` 不用无限 `append()`；环形缓冲固定窗口；降采样。

### 12.4 视频面

优先 **`qml6glsink`**（前提是发行版提供 Qt6 版 GStreamer sink）；**但大概率走 `appsink` + `QQuickFramebufferObject` 手动导入 dmabuf**——Ubuntu 长期只打包 Qt5 版 sink，需在环境验证阶段用 `gst-inspect-1.0 qml6glsink` 确认。**不用 QtMultimedia**（无法控制低延迟）。

### 12.5 可主题化（为开源交流）

目标不是插件 ABI，而是**可 fork 的 QML 目录**：

```
src/ui/qml/
  main.qml
  Theme.qml           # 颜色/字号/间距，单例
  panels/
    VideoPanel.qml  TelemetryPanel.qml  MapPanel.qml
  panels.json         # channel → 组件 + 布局
src/ui/theme/dark.json light.json
```

三条纪律：

1. **C++ 不硬编码布局与颜色**，全部来自 `Theme.qml` 与 `panels.json`；
2. 提供 `--ui-dir <path>` **运行时加载外部 QML 目录**（改主题不用重编译）；
3. 文档写清"如何加一个面板"（十行以内）。

---

## 13. 可靠性：可观测的尽力而为

### 13.1 口径

**我们承诺：**

| 承诺 | 实现 |
|---|---|
| 自动恢复 | 通信进程 systemd `Restart=always, RestartSec=0.2s`；前端重开即重连；IPC 断开指数退避 |
| **不放大问题** | ① 不阻塞上游 ② 不重发旧状态 ③ 不喂残帧 |
| 一切丢弃可见 | 每处覆盖计数，UI 常驻显示，录制打标 |
| 延迟可测 | RMCS 产出 → publish 返回，p95/p99 常驻 |

**我们不承诺：**

- 不承诺零中断（机器人侧负责失效处理，策略待定）；
- 不承诺不丢帧（网络与调度不可控）；
- 不承诺远端能感知丢包（proto 无序号，物理上做不到）。

### 13.2 失效矩阵

| 故障 | 检测 | 上行控制 | 7 条指令 | 数据录制 | 视频录制 | 恢复 |
|---|---|---|---|---|---|---|
| 通信进程崩溃 | systemd / 启动器 | ❌ 中断 | ❌ | 断 | 断 | 拉起后重连 |
| 前端进程崩溃 | 通信进程检测 UDS 断开 | ✅ 不受影响 | ❌ 发不出 | ✅ 继续 | ❌ 断（留缺口） | 重开前端，接着录 |
| 前端慢/卡死 | 通信进程 `EAGAIN` | ✅ 不受影响 | ❌ | ✅ 继续 | ❌ 掉帧 | 恢复后追平 |
| AT-Bridge 重启 | `POLLHUP` | ❌ 无控制 | ✅ 仍可发 | 打标 | 无影响 | 指数退避重连 |
| MQTT / 服务器断开 | Paho 回调 | ❌ 发不出 | ❌ | ✅ 继续 | ✅ 继续 | 自动重连 |
| 网络中断 | 同上 | ❌ | ❌ | ✅ | ✅ | 同上 |
| 整机断电 | — | ❌ | ❌ | 断 | 断 | 全部自启，分段可恢复 |

> "机器人行为"一列不在本文范围——由机器人控制方按其失效策略处理。

### 13.3 验收项

| 指标 | 目标 |
|---|---|
| RMCS 产出 → publish 返回 | p99 < 2 ms |
| 桥接队列深度 | 恒为 1，无增长 |
| **慢消费者测试** | 前端故意慢读 10 s：通信进程不受影响，丢弃计数正确，恢复后追平 |
| 连续 `kill -9` 前端 20 次 | 控制链路零中断，数据录制无缺口 |
| `kill -9` 通信进程 | 0.2 s 内拉起，重连恢复 |
| 24 h soak | fd 与内存无增长，无丢弃（正常网络） |
| `kill -9` 后回放 | 前 N 段完整可播，manifest 可恢复 |
| UI 帧率 | 60 fps，10 min 掉帧 < 1% |

---

## 14. 工程规范

### 14.1 目录

```
teleop-client/
├─ LICENSE                     # GPL-3.0-or-later（src/）
├─ LICENSE-APACHE              # Apache-2.0（proto/ libs/ tools/）
├─ REUSE.toml
├─ CMakeLists.txt / CMakePresets.json
├─ proto/                      # Apache-2.0；官方协议的转录实现
│  ├─ rm/*.proto               # 32 条官方消息（字段唯一真源）
│  ├─ topics.inc               # 32 个 topic：编号/名字/QoS/谁能发
│  ├─ ipc.proto  ui_link.proto
├─ libs/teleop_ipc/            # Apache-2.0，独立 target（给 AT-Bridge 用）
├─ src/
│  ├─ core/    # 时钟、日志、错误、配置、LatestValueRegister、Fd RAII
│  ├─ ipc/     # uds_seqpkt、framing、session、channel 字典
│  ├─ mqtt/    # paho 封装、单槽发布、重连
│  ├─ record/  # sidecar writer、manifest
│  ├─ node/    # 通信进程 main（组装 ipc/mqtt/record/ui_server）
│  ├─ video/   # UDP 收包、重组器、gst 管线、dmabuf 桥
│  ├─ replay/  # ReplaySource、虚拟时钟、seek
│  ├─ geo/     # WGS84↔ECEF↔ENU
│  ├─ scene/   # 3D 地图、相机、标记（v1 不做，留位）
│  ├─ ui/      # QML、Theme、panels、视频面
│  ├─ app/     # 前端 main
│  └─ uilink/  # 展示链路 client
├─ assets/                     # Git LFS
├─ tools/teleop_sim/           # Apache-2.0，AT-Bridge 参考实现 + 故障注入
├─ tools/referee_sim/          # Apache-2.0，假裁判系统：发 23 条下行 / 收 9 条上行 / 假图传
├─ tests/  unit / integration / fuzz
├─ docs/   architecture.md  ipc-protocol.md  adr/
└─ packaging/ appimage/ deb/ docker-dev/
```

### 14.2 构建

- CMake ≥3.25 + Ninja + CMakePresets；
- Qt6 / GStreamer 用发行版包；
- **protobuf 用发行版 3.21**（24.04 自带，满足对端 ≥3.15；将来对端要求更高版本再引 Conan）；
- spdlog / toml++ / GTest 用发行版包（24.04 均有）或 FetchContent；**v1 不引入 Conan**，减少一层工具链；
- **提供 Docker 开发镜像**。

### 14.3 C++ 硬性规范

| # | 规范 | 原因 |
|---|---|---|
| 1 | `signal(SIGPIPE, SIG_IGN)` + `MSG_NOSIGNAL` | 对端关闭会杀进程 |
| 2 | `socket()` 带 `SOCK_NONBLOCK\|SOCK_CLOEXEC`；`accept4()` 同理 | fork 泄漏窗口 |
| 3 | fd 用 RAII | 泄漏/重复关闭 |
| 4 | 热路径零分配 | 延迟抖动 |
| 5 | 热路径禁日志/异常/`std::function` | 隐式分配与加锁 |
| 6 | `SO_PEERCRED` 校验 | 访问控制 |
| 7 | `recvmsg` + `MSG_TRUNC` | SEQPACKET 超长帧静默截断 |
| 8 | 信号用 `signalfd` | 信号处理函数内不能做事 |
| 9 | `steady_clock` 管间隔、`system_clock` 只管墙钟 | NTP 跳变 |
| 10 | `noexcept` 移动 | vector 扩容退化 |
| 11 | 禁止 `-march=native` | 换机器 `SIGILL` |
| 12 | QML 只通过 60 Hz 快照收数据 | 事件循环被灌爆 |
| 13 | **转发路径不得修改 `payload`** | 客户端永远是"桥" |

### 14.4 编译选项

```
-std=c++20 -O2 -flto -march=x86-64-v2
-Wall -Wextra -Werror
-fstack-protector-strong -D_FORTIFY_SOURCE=2
-Wl,-z,relro,-z,now
```

CI 额外跑 ASan/UBSan/TSan。

### 14.5 基础组件

| 用途 | 选型 |
|---|---|
| 事件循环 | 裸 `epoll` + `eventfd` |
| 并发 | `std::thread` + `std::atomic` + `LatestValueRegister` |
| 时间 | `clock_gettime(CLOCK_MONOTONIC)` |
| 日志 | spdlog（async） |
| 配置 | TOML（toml++） |
| 错误 | `std::expected` / `tl::expected` |
| 序列化 | protobuf（发行版 3.21） |
| 压缩 | zstd |
| 指标 | HDR 直方图 + 调试面板 |

### 14.6 测试

| 层 | 手段 |
|---|---|
| 单元 | GoogleTest + CTest |
| 协议 | libFuzzer 打帧头/payload 边界 |
| **图传重组** | 桩发送端：乱序、丢分片、重复分片、帧号回绕、发送端重启 |
| 集成 | `teleop_sim` + 故障注入 |
| **慢消费者** | 前端故意慢读 10 s，验证不影响通信进程 |
| 长稳 | 24 h soak |

### 14.7 开发容器

开发、单测、集成测试、CI 全部在容器里跑，**GUI 也在容器里**。详细决策与验证清单见独立文档 `teleop-client-dev-environment.md`，此处只摘录必须记住的结论：

| 项 | 结论 |
|---|---|
| 基线 | **Ubuntu 24.04**（GStreamer 1.24 / protobuf 3.21 / Qt 6.4） |
| 硬性前提 | 容器与**宿主同发行版**（Mesa/libva ABI）；容器内 **uid = 宿主 uid**；**挂宿主 `/dev/dri` 与 `dri` 驱动，容器不装驱动** |
| 必须参数 | `--network=host`（否则 netns 的 `rmem_max` 默认 208 KB 会卡死 60 fps 收包）、`ipc=host`、`--device /dev/dri`、`--group-add <render gid>` |
| 绝对不要 | **设 CPU 配额**（CFS quota 会 throttle，最坏 100 ms） |
| 部署 | **不带容器**：裸机 + systemd + `.deb`/AppImage |

三条使用规则：

1. **延迟只用 Release 构建测**；sanitizer 构建只用来找 bug；
2. 容器内外可用同一套打点直接对比（`CLOCK_MONOTONIC` 是共用的）；
3. **最终验收在真机跑一次**——驱动、合成器、systemd 调度只在真机上完整。

---

## 15. 许可证与合规

### 15.1 分层授权【定稿】

| 目录 | 许可证 | 理由 |
|---|---|---|
| `src/` | **GPL-3.0-or-later** | 保护应用 |
| `proto/` | **Apache-2.0** | 协议要被 RMCS 项目自由实现 |
| `libs/teleop_ipc/` | **Apache-2.0** | 要被链接进 RMCS 项目 |
| `tools/teleop_sim/` | **Apache-2.0** | 鼓励他人改造 |
| `assets/` | 自有 / CC-BY-4.0 | 与 GPLv3 兼容 |

**一句话：应用闭源保护，协议开放互通。**

选 GPLv3 的附带收益：Qt 可按 GPLv3 选项消费，解除"必须动态链接"约束，并解锁 Qt Charts。

### 15.2 依赖许可

| 组件 | 许可 |
|---|---|
| Qt 6 / Qt Charts | LGPLv3 / GPLv3 / 商业 |
| GStreamer base/good/bad | LGPL |
| protobuf / spdlog / toml++ / GTest | BSD / MIT |
| Paho MQTT C | EPL-2.0 / EDL |
| cgltf / meshoptimizer | MIT |

### 15.3 专利

开源不豁免专利。H.265 解码涉及专利池；本项目为非商业交流用途，风险低；若将来商业化需单独评估。写入 `docs/adr/`。

### 15.4 禁令

- ❌ 静态链接专有组件；
- ❌ 引入 `gst-plugins-ugly`（除非确需）；
- ❌ 在 `proto/`/`libs/` 写 GPL 头；
- ❌ 在转发路径解析或改写业务 payload。

---

## 16. 分发与社区

### 16.1 三件套【定稿】

| 渠道 | 内容 |
|---|---|
| 源码 | GitHub + CMakePresets + Docker 开发镜像 |
| Releases | **AppImage**（+ 可选 `.deb`）+ `SHA256SUMS` |
| 样例会话 | 一段录制数据（视频 + sidecar + manifest），Release 资产 |

不做：Flathub、Snap、apt 仓库、自动更新、商业签名。

**部署形态不带容器**：产物在裸机上以 `systemd` 运行（通信进程为系统服务，前端为桌面应用）。容器只用于开发、测试与 CI，见 §14.7。理由是 VA-API 驱动与内核/GPU 强绑定、systemd 生命周期、以及 UDS 权限对齐——容器化在现场只增加故障面，换不来收益。

### 16.2 打包三坑

1. GStreamer 插件路径：包内打包须设 `GST_PLUGIN_SYSTEM_PATH=""`，更稳的做法是**依赖系统 GStreamer**（最低 1.22）；
2. **绝不打包 VA-API 驱动**；
3. Qt 平台插件：必须同时带 `libqxcb.so` 与 `libqwayland*.so`。

### 16.3 CI

GitHub Actions：**Ubuntu 24.04 为基线**（与开发容器同一镜像、同一 digest），GCC/Clang 双编译器；ASan/UBSan/TSan；fuzz 冒烟；tag 触发 Release；`reuse lint`。

### 16.4 社区

DCO（不用 CLA）；SPDX 头 + `REUSE.toml`；README 双语；`ipc-protocol.md` 公开；SemVer + 统一提交信息格式（英文、首字母大写、正文列小点，见《施工计划》§5）。

### 16.5 为"交流"必须有的三样

1. **`teleop_sim`**：AT-Bridge 的参考实现（75 Hz 快照、下行数据块、故障注入），Apache-2.0；
2. **`referee_sim`**：假裁判系统（发 23 条下行、收 9 条上行、假图传），Apache-2.0；
3. **样例会话**：`--replay sample_session/` 一条命令即可看完整效果。

```bash
./teleop-client --replay sample_session/   # 无网络、无硬件、无后端
./teleop-client --sim                      # 接 teleop_sim + referee_sim
./teleop-client                            # 接真实裁判系统
```

---

## 17. 性能预算

| 环节 | 典型 | 上限 |
|---|---|---|
| IPC 单程 | 30–100 µs | 300 µs |
| 展示链路单程 | 30–100 µs | 300 µs |
| **RMCS 产出 → publish 返回** | **0.3–1.2 ms** | **p99 < 2 ms** |
| 对照：75 Hz 周期 | 13.3 ms | — |
| 图传：收包 + 重组 | 0.2–1 ms | 3 ms |
| 图传：硬解 1080p60 | 4–10 ms | 15 ms |
| 图传：呈现 | 0–16.7 ms | 16.7 ms |
| **图传 glass-to-glass** | **10–35 ms** | **≤60 ms** |

**结论：客户端不是瓶颈。** 资源分配顺序：**视频质量与稳定性 > 桥接可靠性 > 桥接延迟**。

---

## 18. 里程碑

| 阶段 | 交付 | 出口条件 |
|---|---|---|
| **M0 环境验证**（2–3 天） | `vainfo`、图传收包抓包脚本、重组器 spike | 能稳定收到 60 fps 完整帧；测出解码耗时 |
| **M1 控制链路**（1 周） | `ipc.proto`、`libs/teleop_ipc`、`teleop_sim`、`ipc-protocol.md` | sim 跑通 75 Hz 快照 + 命令；慢消费者测试通过 |
| **M2 通信进程**（1 周） | IPC client、MQTT 单连接、按类分发、数据录制、`ui_link.proto` | 产出→publish p99 < 2 ms；断连丢帧计数正确 |
| **M3 前端**（2 周） | 视频管线 + 重组器、面板框架、展示链路 client、7 条指令 | 1080p60 稳定显示；75 Hz 数据下 60 fps |
| **M4 录制回放与发布**（1–2 周） | 会话目录、两个录制器、`ReplaySource`、样例会话、AppImage | `--replay` 一条命令可演示 |

**优先级**：先让 `--replay` 能跑（门面），再补实时链路。

> 3D 场地地图移出 v1（见 §11.1），相应工作与"地图资产"依赖一并推迟。

---

## 19. 待定项（外部对接，不阻塞实现）

| # | 事项 | 何时需要 | 状态 / 不阻塞原因 |
|---|---|---|---|
| 1 | `RadarInfoToClient` 外层字段真伪 | 联调前 | 官方原文有误（类型 uint32、缺 tag）；先按 `repeated RadarSingleRobotInfo = 1` 实现，联调时验证，必要时加容错解码 |
| 2 | 机器人超时 T 与失效动作 | 不影响实现 | 只影响 UI 告警阈值，可配置 |
| 3 | 图传 IDR 间隔 / repeat-headers / 位深 | 不影响实现 | 运行时测量并显示 |
| 4 | 官方协议版本更新 | 每年赛季 | 32 个 topic 或字段变化时，同步 `proto/` 与静态表 |
| 5 | RMCS 容器的 uid 与 bind mount | M2 之前 | 见 §4.4「容器侧三个前提」，需要 RMCS 项目配合 |
| 6 | 联调环境 | M2 之后 | 已有裁判系统可联调；新官方选手端发布后才能对照抓包 |

> ~~原第 1–4 项（通道映射 / 完整 proto / retained / clientId 与认证）~~ 已于 v1.4 全部关闭：协议固定 32 个 topic、无认证、不用 retained。

---

## 20. 附录

### 20.1 术语

| 术语 | 含义 |
|---|---|
| ENU | 东-北-天局部坐标，米，Z-up |
| SEQPACKET | 面向连接、可靠、保消息边界的 Unix socket 类型 |
| 全局状态快照 | 每帧包含全部状态的完整表示，非增量 |
| latest-wins | 拥塞时覆盖旧值而非排队 |
| 单槽 | 队列深度为 1，写入即覆盖 |
| 重组器 | 把 UDP 分片拼成完整 H.265 帧的模块 |
| glass-to-glass | 从摄像头出图到屏幕显示的总延迟 |
| sidecar | 与视频文件并列的数据文件 |
| dmabuf | Linux DMA-BUF，GPU 缓冲零拷贝共享 |

### 20.2 参考

- MQTT 3.1.1 — OASIS Standard
- ITU-T H.265 / ISO 23008-2（HEVC）
- GStreamer：`splitmuxsink`、`h265parse`、`vah265dec`
- Qt：Qt Quick 3D、`View3D.pick()`、QML 场景图
- Linux：`unix(7)`、`epoll(7)`、`dma-buf`
- Wayland：`zwp_linux_dmabuf_v1`
- Matroska 容器规范

### 20.3 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-11-17 | 首次定稿 |
| v1.1 | 2026-11-18 | 定位修正为"桥"：删输入子系统、控制帧语义、deadman、双通道；IPC 单连接 + 不透明 payload；全量快照 + 单槽 + 断连不缓冲 |
| **v1.2** | **2026-11-18** | **三进程架构**（RMCS/通信/前端）；客户端进入命令回路；新增展示链路与 `ui_link.proto`；录制跨进程按 session 目录拆分；图传改为自定义 UDP 协议 + 自写重组器；**MQTT 取消信封**（裸 protobuf）；事件降级为 channel 的 `qos` 属性；命令去重/过期/业务回执归 RMCS；可靠性口径改为"可观测的尽力而为"；新增十条不变量、失效矩阵、视频运行时测量、UI 可主题化 |
| **v1.3** | **2026-11-19** | **环境落地回填**：基线定为 Ubuntu 24.04；Qt 6.8 → **发行版 6.4**（无 `QQuickRhiItem`，视频面走 `QQuickFramebufferObject`）；protobuf Conan → **发行版 3.21**；CI 基线统一 24.04；新增 **§14.7 开发容器**（GUI 进容器、三个前提、延迟影响）；明确**部署不带容器**；移除"发行版基线"待定项 |
| **v1.4** | **2026-11-19** | **协议落地回填**：方向口径统一为"上行=发往服务器/机器人"；协议固定 32 个 topic、单连接、clientID=机器人编号、无认证、最高 QoS1；**RMCS 只保留 `CustomControl`/`CustomByteBlock` 两条通道**（IPC 从 32 缩到 2）；**前端可发 7 条触发性指令**；键鼠 v1 不实现；录制改为上行/下行/视频三份、不录 IPC；删除 `veh/{id}/status`（LWT/retained）；3D 场地地图移出 v1 |
| **v1.5** | **2026-11-19** | **部署与桌面壳**：启动模型定为用户级（`systemd --user` + 桌面程序），不建系统用户；socket 移到 `~/.local/state/teleop/run/`，控制 socket 单独子目录 bind mount 进 RMCS 容器；新增 **§4.5 托盘与进程控制**（点 X 隐藏、菜单五项、退出连带停进程、关闭/退出两处二次确认、比赛中提示、托盘不可用兜底） |
