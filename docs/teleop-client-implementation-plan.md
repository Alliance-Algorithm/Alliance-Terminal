# 遥控客户端 · 施工计划

| 项 | 内容 |
|---|---|
| 文档版本 | v1.4 |
| 状态 | **施工中**（B0、B1、B5a、B2 已完成，B2b 未开始；进度见第 9 节） |
| 日期 | 2026-11-20 |
| 关联文档 | `teleop-client-architecture.md`（v1.6）、`custom-terminal-protocol.md`、`teleop-client-dev-environment.md`（v0.3）、`ipc-protocol.md`（v1.1） |

> 本文是"**怎么建**"的施工图：顺序、目录、批次、验收标准。
> "**为什么这么设计**"在架构文档里，本文不重复。
> 规则：**每批做完更新第 9 节，再进入下一批**；改动批次划分要写进第 10 节。

---

## 0. 一页结论

| 项 | 结论 |
|---|---|
| 施工顺序 | **先把 IPC 两端闭环，再做其余**：协议层 → IPC 库 → 客户端 IPC 策略 + 终端桥 → 两端联调 → 模拟器 → 通信进程的 MQTT/录制/UI；Qt 与视频放到 M0/M3 |
| 批次 | **B0 ~ B5 + B2b + I0**；桥仓 D0–D5 是外部并行批次 |
| 工期估算 | B0 0.5、B1 1.5、B5a 0.5、B2 1.5、B2b 1.5、I0 0.5–1、B3 1、B4 2.5、B5b 0.5（客户端合计 6–7 天；桥仓约 3 天，并行） |
| 优先目标 | **先**让 `ipc_probe` ↔ 终端桥跑通 75/50 Hz、重连、故障注入；**再**用模拟器跑通 MQTT 端到端并落出 `mqtt_up.pb.zst` / `mqtt_down.pb.zst` |
| v1 明确不做 | 3D 地图、键鼠输入、多客户端、音频、云端 |

---

## 1. 施工顺序与依赖

```
B0 骨架
  └─► B1 协议层（proto + topic 表 + 编解码库）
        ├─► B5a 构建/测试容器（Dockerfile + dev-shell + CI 骨架）
        │     └─► B2 teleop_ipc（自包含库）
        │           ├─► B2b 客户端 IPC（core 最小集 + ipc_client + ipc_probe）
        │           │        └─► I0 两端联调（客户端 ↔ 终端桥）
        │           └─► 桥仓 D0–D5（外部，vendor snapshot）
        │                    └─► I0（与 B2b 汇合）
        │                          └─► B3 两个模拟器 ──► B4 通信进程（MQTT/录制/UI）
        │                                                  └─► B5b GUI/GPU 容器 ──► M0 视频 spike ──► M3 前端
        └─► （B3 的 MQTT 侧也可以只依赖 rm_proto，但 IPC 两端优先）
```

> **当前优先**：B2 完成后，B2b 与桥仓 D0–D3 **并行**；I0 绿之前不开始 B3/B4。桥仓不是本计划的批次，D0–D5 见桥仓 `docs/design.md`。

**硬约束（每批都要守）**：

1. `teleop-node` 的依赖树里不出现 Qt / GStreamer（CMake target 依赖天然保证 + CI grep 兜底）；
2. 转发路径不改 payload 字节（`bytes` 原样进原样出）；
3. **客户端不解 payload**：`UpData.payload` / `DownData.payload` 是官方 envelope 的序列化字节，只有终端桥组/解；客户端库不包含 `CustomControl`/`CustomByteBlock` codec；
4. 单槽：`LatestValueRegister` 之外不允许出现无界队列；
5. 每批结束时 `ctest` 必须全绿，且 `cmake --preset release` 能编过。

---

## 2. 目录树

★ = 第一批（B0–B5）会真正写下代码的目录；其余是预留位。

```
Alliance-Terminal/
├─ LICENSE ★                     GPL-3.0-or-later（src/）
├─ LICENSE-APACHE ★              Apache-2.0（proto/ libs/ tools/ 及构建脚本）
├─ REUSE.toml ★                  SPDX 归属表
├─ README.md ★                   双语
├─ CMakeLists.txt ★              顶层，按批次逐步打开子目录
├─ CMakePresets.json ★           dev / release / ci / asan / tsan
├─ .clang-format ★               （与 RMCS 同风格）
├─ .clang-tidy ★
├─ .editorconfig ★
├─ .gitignore ★
├─ proto/ ★                      Apache-2.0，字段唯一真源
│  ├─ rm/terminal.proto          32 条官方消息，package rm.terminal
│  ├─ topics.inc                 32 个 topic 的 X-macro 表
│  ├─ ipc.proto                  Hello / HelloAck / UpData / DownData / Health
│  └─ ui_link.proto              UiHello / UiData / UiCmd / UiHealth / ...
├─ libs/
│  ├─ rm_proto/ ★                Apache-2.0：语义 ↔ 字节（header-only）
│  │  └─ include/rm_proto/{topics,codec}.hpp
│  └─ teleop_ipc/ ★              Apache-2.0：帧头 + SEQPACKET + 握手 + IPC codec（header-only、自包含）
│     └─ include/teleop_ipc/{frame,seqpacket,session,control_link,wire}.hpp
├─ src/
│  ├─ core/ ★                    时钟 / 日志 / 错误 / 配置 / LatestValueRegister / Fd RAII
│  ├─ ipc/ ★                     连终端桥：单槽、健康、重连；framing/session 用 teleop_ipc
│  ├─ mqtt/ ★                    Paho 封装 / 单槽发布 / 指令队列 / 限速
│  ├─ record/ ★                  mqtt_up、mqtt_down 写入器 + manifest
│  ├─ node/ ★                    通信进程 main + epoll 事件循环
│  ├─ uilink/ ★                  展示链路 server 端（node 侧）
│  ├─ app/                       前端 main + 托盘（M3）
│  ├─ ui/                        QML / Theme / panels（M3）
│  ├─ video/                     UDP 收包、重组器、GStreamer 管线（M0/M3）
│  ├─ replay/                    ReplaySource（M4）
│  ├─ geo/                       WGS84 ↔ ECEF ↔ ENU（v1 不做）
│  └─ scene/                     3D 地图（v1 不做）
├─ tools/
│  ├─ ipc_probe/ ★               独立 IPC 探针（无 protobuf；客户端 IPC 端联调用）
│  ├─ teleop_sim/ ★              假终端桥（75 Hz / 50 Hz + 故障注入）
│  └─ referee_sim/ ★             假裁判系统（发 23 条、收 8 条、假图传）
├─ tests/
│  ├─ unit/ ★
│  ├─ integration/ ★
│  └─ fuzz/                      M1 之后
├─ packaging/
│  ├─ systemd/teleop-node.service ★
│  └─ appimage/                  M4
├─ docker/ ★                     Dockerfile（base/dev/ci）+ dev-shell.sh + compose
├─ scripts/ ★                    sim-demo.sh、fmt.sh
├─ .github/workflows/ci.yml      代码能编之后再配
└─ docs/                         五份文档 + adr/（含 ipc-protocol.md）
```

> 终端桥 `rmcs_terminal_bridge` 是 RMCS 仓里的独立子模块，**不在本树、不列入本计划的批次**；本计划只保证 `teleop_ipc` 是自包含子目录；桥仓 vendor 一份快照，线格式契约见 `docs/ipc-protocol.md`。

---

## 3. 构建拓扑与硬约束

```
rm_proto ────────┬──► teleop_sim
                 ├──► referee_sim
                 └──► teleop_ui（M3）

teleop_ipc ──────┬──► teleop_sim
                 ├──► teleop_node
                 ├──► ipc_probe（经 src/ipc/ipc_client）
                 └──► rmcs_terminal_bridge（外部仓，vendor snapshot）

core ──┬──► ipc ──┬──► teleop_node
       │          └──► ipc_probe
       ├──► mqtt ─────┼──► teleop_node
       ├──► record ───┤
       └──► uilink ───┘

core + uilink + rm_proto ──► teleop_ui（M3，才拉 Qt / GStreamer）
```

| 约束 | 落地方式 |
|---|---|
| 通信进程不依赖 UI 框架 | CMake target 依赖里根本没有 Qt/GStreamer；CI 再 `grep -R "Qt\|gst" src/{core,ipc,mqtt,record,node,uilink}` 兜底 |
| 协议层可被别的项目复用 | `proto/`、`libs/` 用 Apache-2.0，且不依赖 `src/` |
| `teleop_ipc` 可被桥仓消费 | 库自包含：不调 `project()`、不用顶层 target、测试不进库的 CMake；独立 CMake 工程能 `add_subdirectory` |
| 客户端不解析 payload | `libs/teleop_ipc` 不包含 `CustomControl`/`CustomByteBlock` codec；节点只搬 `UpData.payload`/`DownData.payload`；代码评审兜底 |
| 前端不能碰控制链路 | `uilink` 里不存在通往 `ipc` 的调用；代码评审 + 单测断言 |

---

## 4. 技术选择

| # | 选择 | 结论 | 理由 |
|---|---|---|---|
| 1 | proto package | `rm.terminal`（官方 32 条）、`teleop.ipc`、`teleop.ui` | 官方未给包名；只影响命名空间，不影响线格式 |
| 2 | topic 编号 | **上行 1–9、下行 10–32**（按整理稿顺序） | 本地概念，固定后录制/调试/UI 三处一致 |
| 3 | protobuf 代码 | **构建时生成**，不提交生成物 | 避免与 `.proto` 不一致 |
| 4 | 错误类型 | `tl::expected` **vendored 单头文件**（v1.3.1，CC0-1.0） | 容器没有代理时 FetchContent 会卡在 configure；vendor 可离线、可复现 |
| 5 | 第三方库 | protobuf / spdlog / GTest / Paho **用系统包**；toml++ FetchContent | 24.04 仓库都有；toml++ 是轻量头文件库 |
| 6 | MQTT | `MQTTAsync` | 75 Hz 单槽需要非阻塞发布 |
| 7 | 事件循环 | 裸 `epoll` + `eventfd` | 架构 §14.5 |
| 8 | 日志 | spdlog（async，热路径禁日志） | 架构 §14.5 |
| 9 | 代码组织 | **header-only**：一个组件 = 一个 `.hpp`，库做成 `INTERFACE` target；只有 `main.cpp`、测试文件、`protoc` 生成物是 `.cpp` | 便于阅读与跳转；避免声明/实现漂移 |
| 10 | IPC 编解码 | **手写** `control_link.hpp` + `wire.hpp`，不依赖 protobuf；测试用 `ipc.pb` 的字节对照 | RMCS 镜像没有 protobuf 开发包；桥侧无法 `protoc` |
| 11 | 终端桥 | **独立仓** `rmcs_terminal_bridge`（GPL-3.0-only）+ RMCS 子模块；vendor `libs/teleop_ipc` 快照（Apache-2.0，保留来源/许可证） | 桥不属于客户端仓；客户端只保留线格式契约 |

---

## 5. 命名与路径约定

| 项 | 约定 |
|---|---|
| 二进制 | `teleop-node`（通信进程）、`teleop-client`（前端）、`ipc-probe`（IPC 联调/故障注入）、`teleop-sim`、`referee-sim` |
| systemd unit | `teleop-node.service`（用户级） |
| socket | `~/.local/state/teleop/run/rmcs/control.sock`、`~/.local/state/teleop/run/ui.sock` |
| 配置 | `~/.config/teleop/node.toml`、`~/.config/teleop/ui.toml` |
| 录制 | `~/.local/share/teleop/sessions/<YYYYMMDD_HHMMSS>/` |
| 日志 | journald（`journalctl --user -u teleop-node`） |
| C++ 命名空间 | `libs/teleop_ipc` 用 **`teleop::link`**（避免与 `ipc.pb.h` 生成的 `teleop::ipc` 同名类型冲突）；`src` 用 `teleop::core` / `teleop::ipc` / `teleop::mqtt` / `teleop::record` / `teleop::uilink`；协议代码 `rm::terminal` |
| IPC 客户端 | `teleop::ipc::IpClient`（生产策略，header-only；内部用 `teleop::link`）；`tools/ipc_probe` 只是可执行外壳；不依赖 protobuf |
| 终端桥 | 包 `rmcs_terminal_bridge`（RMCS 子模块）；组件 `TerminalBridgeRx`（owner）/ `TerminalBridgeTx`（partner）；YAML 实例 `terminal_bridge`；接口 `/terminal/custom_control`、`/terminal/custom_byte_block` |
| 线格式文档 | `docs/ipc-protocol.md`（客户端与桥的共同契约） |
| 提交信息 | **英文**；首行首字母大写、直接用动词说清本轮改了什么（`Add ...` / `Fix ...` / `Update ...` / `Remove ...`），≤ 72 字符；不写 `feat:`/`chore:` 之类前缀；正文用短横线列小点；模板见 `.gitmessage` |
| 代码组织 | **header-only**：组件 = 单个 `.hpp`，不建 `.hpp`/`.cpp` 配对；库用 CMake `INTERFACE` target |
| 提交时机 | **不自动提交**：改动留在工作区，由用户决定何时 commit / push |
| 分支与 PR | **不许直接推 `main`**（受保护）：一律开分支 → 测试全绿 → PR → squash 合并；细则见 `Agent.md` §3.7 |

---

## 6. 批次计划

### B0 骨架（0.5 天）

| 项 | 内容 |
|---|---|
| 目标 | 一个能配置、能提交的空仓库 |
| 交付物 | `LICENSE`、`LICENSE-APACHE`、`REUSE.toml`、`CMakeLists.txt`、`CMakePresets.json`、`.clang-format`、`.clang-tidy`、`.editorconfig`、`.gitignore`、`README.md`、`git init` |
| 出口条件 | `cmake --preset dev` 配置成功；首个提交入仓 |
| 验收 | `cmake --preset dev && cmake --preset release` 均无报错；`git log --oneline` 有一条 |

### B1 协议层（1.5 天）

| 项 | 内容 |
|---|---|
| 目标 | 32 条消息可编解码，topic 表唯一 |
| 交付物 | `proto/rm/terminal.proto`、`proto/topics.inc`、`proto/ipc.proto`、`proto/ui_link.proto`、`libs/rm_proto/**`、`tests/unit/test_topics.cpp`、`tests/unit/test_codec.cpp` |
| 出口条件 | `ctest` 绿：32 条齐全、id 唯一、名字唯一、方向/QoS 正确；7 条指令编码字节正确；23 条 decode 回环 |
| 验收 | `ctest -R topics` 与 `ctest -R codec` 全绿 |

### B5a 构建/测试容器（0.5 天）

| 项 | 内容 |
|---|---|
| 目标 | 所有编译与测试都在 Ubuntu 24.04 容器里跑，换台机器结果一致 |
| 交付物 | `docker/Dockerfile`（base/dev/ci 三阶段）、`docker/build.sh`、`docker/dev-shell.sh`、`docker/mosquitto.conf`、`.devcontainer/devcontainer.json`、`.github/workflows/ci.yml` |
| 出口条件 | `./docker/build.sh dev` 成功；容器内 `cmake --preset ci && ctest` 全绿；`build/` 属主是宿主用户 |
| 验收 | `./docker/dev-shell.sh -- bash -lc 'cmake --preset ci && cmake --build --preset ci -j$(nproc) && ctest --preset ci'` |

### B2 IPC 层（1.5 天）

| 项 | 内容 |
|---|---|
| 目标 | `teleop_ipc` 成为**自包含**的 Apache-2.0 header-only 库：帧头、SEQPACKET、session、控制链路 IPC codec；节点/模拟器/终端桥都能消费 |
| 交付物 | `libs/teleop_ipc/**`（`frame`/`seqpacket`/`session`/`control_link`/`wire` + `third_party/tl/expected.hpp`）、`tests/unit/{test_frame,test_seqpacket,test_session,test_control_link,test_ipc_headers}.cpp`、`docs/ipc-protocol.md` |
| 出口条件 | 32 字节帧头 **golden bytes**（大端）往返正确；截断/超长/`body_len` 不符被拒；抽象/路径 socket 上发一帧收一帧；握手/seq/session 状态正确；手写 IPC codec 与 `ipc.pb` 逐字节一致；`payload` 0/60/300/1400 B 原样往返；库能在**独立 CMake 工程**里 `add_subdirectory` 后编过一个最小 TU |
| 验收 | 五个测试全绿（命令见下）；容器内 `dev`、`ci`（`-Werror`）全绿 |

> 本批**不实现**终端桥；桥的 Rx/Tx 组件、官方 envelope codec、rmcs-runtime 部署属于桥仓自己的计划（见桥仓 `docs/design.md`）。
>
> `docs/ipc-protocol.md` 是与桥仓共用的文档：两边各一份、内容一致；修改时必须同时改对端（见 `Agent.md` §3.8）。

```bash
ctest -R 'frame|seqpacket|session|control_link|ipc_headers'
```

### B2b 客户端 IPC 策略（1.5 天）

| 项 | 内容 |
|---|---|
| 目标 | 客户端 IPC 端成为**可运行的进程**：连接/退避、握手、上行单槽、健康、下行发送、计数；不碰 MQTT/录制/UI |
| 交付物 | `src/core/{clock,error,latest_value_register}.hpp`（最小集，不拉 spdlog/toml）、`src/ipc/ipc_client.hpp`、`tools/ipc_probe/**`（独立可执行，**不依赖 protobuf**）、`tests/unit/test_ipc_client.cpp` |
| 出口条件 | `ipc_probe` 能连上假 peer/桥：握手成功；收 75 Hz `UP_DATA`、发 50 Hz `DOWN_DATA`；`EAGAIN` 丢帧计数；断线退避重连；健康超时；上行单槽覆盖计数正确；`payload` 只搬不解析 |
| 验收 | `ctest -R ipc_client` 全绿；`ipc_probe --socket … --duration 10 --down-hz 50 --stats` 打印握手/速率/计数 |

> `ipc_client` 是生产策略，不是探针专用；B4 的 `teleop-node` 直接复用它，`ipc_probe` 只是可执行外壳 + 故障注入。
>
> `ipc_probe` 是机器人/MQTT 侧的**测试替身**：上行只计数 `UpData.payload`，下行发硬编码的官方 `CustomByteBlock` 样例；它不进入生产路径，也不改变“客户端不解 payload”的约束。

### I0 两端联调（0.5–1 天）

| 项 | 内容 |
|---|---|
| 目标 | 客户端 `ipc_probe` ↔ 真终端桥，分三级：Link 级 → RMCS executor 组件级 → `rmcs-runtime` 运行时 |
| 交付物 | 桥仓 `bridge_link_test`（无 ROS）+ `TerminalTestSource`/`TerminalTestSink`（测试组件）+ `config/terminal_test.yaml`；客户端 `ipc_probe`；联调脚本/记录 |
| 出口条件 | Link 级：握手、75/50 Hz、seq/缺口、`EAGAIN`、重连、版本不匹配、握手超时、截断/超长；组件级：`rmcs_executor` 加载 `TerminalBridgeRx` + 测试源/汇，`ipc_probe` 连上；运行时：`rmcs-runtime` root + 宿主 probe，socket/uid/peercred 符合 §6.3 |
| 验收 | 60 s 稳定；两端计数对齐；kill/restart；慢消费者；`rmcs-runtime` 验证清单全过 |

> B3/B4 在 I0 绿之后开始。桥仓 D0–D5 的顺序与测试见桥仓 `docs/design.md`；本计划不重复。

### B3 两个模拟器（1 天）

| 项 | 内容 |
|---|---|
| 目标 | 没有真实硬件也能产出数据；在 I0 之后开始 |
| 交付物 | `tools/teleop_sim/**`（假终端桥：75 Hz `CustomControl`、50 Hz `CustomByteBlock`、断连/慢消费者注入）、`tools/referee_sim/**`（发 23 条下行、收 8 条上行、假图传）、`scripts/sim-demo.sh` |
| 出口条件 | 两个模拟器各自能独立跑；`referee_sim` 能收到 `teleop_sim` 之外任何客户端发的上行 |
| 验收 | `./scripts/sim-demo.sh --dry-run` 打印双方收发计数 |

### B4 通信进程（2.5 天）

| 项 | 内容 |
|---|---|
| 目标 | 端到端闭环 + 落盘；B2b 的 `ipc_client` 直接复用，本批补 MQTT/录制/uilink/组装 |
| 交付物 | `src/mqtt/**`、`src/record/**`、`src/uilink/**`（server 侧）、`src/node/main.cpp`、`packaging/systemd/teleop-node.service`、`tests/integration/test_end_to_end.cpp`（`src/core`/`src/ipc` 已在 B2b 落地） |
| 出口条件 | `sim-demo.sh` 起 mosquitto + 两个模拟器 + `teleop-node`，产出会话目录且含 `mqtt_up.pb.zst`、`mqtt_down.pb.zst`；单槽覆盖计数正确；`systemctl stop` 优雅退出 |
| 验收 | `ctest -R integration`；`zstd -d` 后能看到记录 |

### B5b GUI/GPU 容器（0.5 天，M0 之前做）

| 项 | 内容 |
|---|---|
| 目标 | 容器里能跑 Qt 界面与 VA-API 硬解，供视频开发用 |
| 前提 | 容器与宿主**同发行版**（Mesa / libva ABI 必须匹配）；宿主 `/dev/dri` 可用 |
| 交付物 | `docker/Dockerfile` 的 `gui` 阶段（重装 Mesa/libva、挂 `/dev/dri`、Wayland/X11 socket）、`docker/dev-shell.sh --gui`、验证清单 |
| 出口条件 | 容器内 `vainfo` 通过、渲染器不是 llvmpipe；`gst-inspect-1.0 vah265dec` 存在 |
| 验收 | 按《开发容器设计文档》第 8 节清单跑一遍 |

---

## 7. 验收与 DoD

**每批的通用 DoD**

1. `cmake --preset dev`、`--preset release`、`--preset ci` 三个都能配置；
2. `ctest --preset dev` 全绿；
3. 新增文件都有 SPDX 头（或已被 `REUSE.toml` 覆盖）；vendored 的 `tl::expected` 有 CC0 标注；
4. 文档第 9 节已更新；
5. 一个提交（或一组小提交）说明清楚做了什么；
6. B2 额外：`libs/teleop_ipc` 在**独立 CMake 工程**里 `add_subdirectory` 后能编过一个最小 TU（证明桥仓可以消费）；
7. B2b 额外：`ipc-probe --socket … --duration 10 --down-hz 50 --stats` 能连上假 peer，打印握手/速率/计数；
8. I0 额外：两端联调的 Link/组件/运行时三级至少各跑一次，记录在桥仓。

**IPC 两端闭环验收（B2 + B2b + 桥仓 D0–D3 做完）**

```bash
ipc-probe --socket ~/.local/state/teleop/run/rmcs/control.sock \
          --duration 60 --down-hz 50 --stats
# 期望：handshake ok；up ≈75 Hz；down ≈50 Hz；steady-state drops/gaps 为 0
```

**第一批整体验收（B0–B4 与 B5a 做完）**

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
./scripts/sim-demo.sh            # 起 broker + 两个模拟器 + node
ls ~/.local/share/teleop/sessions/*/   # 应有 mqtt_up.pb.zst / mqtt_down.pb.zst
```

---

## 8. 风险与逃生口

| 风险 | 影响 | 逃生口 |
|---|---|---|
| 24.04 仓库缺 `libpaho-mqtt-dev` 或版本过老 | B4 卡住 | FetchContent 拉 Paho，或用 `libmosquitto` 顶一阵（接口窄，换起来只动 `src/mqtt`） |
| `protobuf_generate` 与 3.21 的 CMake 模块不兼容 | B1 卡住 | 改用 `protoc` 自定义命令生成，不依赖 `protobuf_generate_cpp` |
| 32 条消息里 `RadarInfoToClient` 原文有误 | 联调时解析失败 | 已按修正版实现；预留"两种都试"的容错分支（B1 里留 TODO） |
| 模拟器与真实协议偏差 | 到现场才发现 | B3 完成后，尽早用真裁判系统跑一次（§19 待定项 6） |
| 单槽实现走样（某处偷偷用队列） | 延迟随运行时长增长 | 代码评审 + B4 的"慢消费者"集成测试 |
| 手写 IPC codec 与 `.proto` 漂移 | 线上字节不一致 | `test_control_link` 用 `ipc.pb` 逐字节对照；改 proto 必须同时改 codec/测试 |
| 桥仓与客户端协议漂移 | 桥编不过或握手失败 | 桥仓 vendor 快照 + 显式同步；`docs/ipc-protocol.md` 是唯一契约；握手版本检查兜底 |
| `rmcs-runtime` root 下 socket 权限 | 节点连不上/peercred 拒绝 | 宿主目录 0700、socket 0666、桥 `allowed_uid=1000`、节点 `allowed_peer_uid=0`；按 dev-environment §6.3 清单验证 |

---

## 9. 进度记录

| 批次 | 状态 | 完成日期 | 备注 |
|---|---|---|---|
| B0 骨架 | ✅ 完成 | 2026-11-19 | 许可证 / REUSE / CMake + 五个预设 / 格式化配置 / README / 首个提交；`cmake --preset dev`、`--preset release` 均通过 |
| B1 协议层 | ✅ 完成 | 2026-11-19 | 32 条消息 + `topics.inc` + 两个内部链路 proto + `rm_proto` 编解码库（已改为 header-only）+ 23 个用例；在 Ubuntu 24.04 容器（protoc 3.21.12）里 `dev` 与 `ci`（`-Werror`）预设均全绿 |
| B5a 构建容器 | ✅ 完成 | 2026-11-19 | `docker/Dockerfile`（base/dev/ci）+ `build.sh` + `dev-shell.sh` + CI 骨架；容器内 `ci` 预 configure/build/ctest 全绿 |
| B2 IPC | ✅ 完成 | 2026-11-20 | `libs/teleop_ipc` 自包含库：frame/seqpacket/session/control_link/wire + vendored `tl::expected`；`test_frame`/`test_seqpacket`/`test_session`/`test_control_link`/`test_ipc_headers`；容器内 `dev` 与 `ci`（含头文件 `-Werror` smoke）全绿；独立 CMake 工程 `add_subdirectory` 可消费 |
| B2b 客户端 IPC | ⬜ 未开始 | — | `core` 最小集 + `ipc_client` + `ipc_probe`；下一步 |
| I0 两端联调 | ⬜ 未开始 | — | 客户端 probe ↔ 终端桥；桥仓 D0–D3 并行；B3/B4 之前 |
| B3 模拟器 | ⬜ 未开始 | — | — |
| B4 通信进程 | ⬜ 未开始 | — | 复用 B2b 的 `ipc_client` |
| B5b GUI/GPU 容器 | ⬜ 未开始 | — | M0 之前做 |

> 状态用 ⬜ 未开始 / 🟡 进行中 / ✅ 完成 / ⛔ 受阻。每批完成后在此追加一行结论（做了什么、没做什么、遗留什么）。
>
> B2 的协议与范围已在 v1.3 定稿并落地（代码见 `libs/teleop_ipc`）；下一步 B2b + 桥仓骨架，终端桥另见其独立仓的文档，不计入本表。当前优先顺序：**B2b + 桥仓 D0–D3（并行）→ I0 → B3/B4**。

---

## 10. 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-11-19 | 首次定稿：六批次划分、技术选择、命名约定、验收标准 |
| v1.1 | 2026-11-20 | IPC 契约与桥边界回填：`teleop_ipc` 自包含 + 手写 IPC codec + `wire`；`tl::expected` vendor；B2 交付物/验收更新；原 AT-Bridge 更名 `rmcs_terminal_bridge` 并移出本计划（独立仓/文档）；新增 `docs/ipc-protocol.md`；rmcs-runtime 部署风险 |
| v1.2 | 2026-11-20 | 调整为 **IPC 两端优先**：新增 B2b（`core` 最小集 + `ipc_client` + `ipc_probe`）与 I0（客户端 probe ↔ 终端桥三级联调）；桥仓 D0–D3 并行；B3/B4 后置；`ipc_probe` 进目录/命名/DoD/进度 |
| v1.3 | 2026-11-20 | B2 落地：`libs/teleop_ipc` 自包含库 + 5 个测试 + 头文件 `-Werror` smoke；库命名空间定为 `teleop::link`（避免与生成的 `teleop::ipc` 冲突）；容器内 `dev`/`ci` 全绿；B2b 待开始 |
| v1.4 | 2026-11-20 | 桥仓改为 **vendored `teleop_ipc`**：整库快照 + `add_subdirectory`，删除 FetchContent/SOURCE_DIR 依赖；共享 IPC 文档升 v1.1；桥仓构建不再访问 Alliance-Terminal |
