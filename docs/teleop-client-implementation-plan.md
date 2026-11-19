# 遥控客户端 · 施工计划

| 项 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 状态 | **施工中**（B0、B1、B5a 已完成，B2 未开始；进度见第 9 节） |
| 日期 | 2026-11-19 |
| 关联文档 | `teleop-client-architecture.md`（v1.5）、`custom-terminal-protocol.md`、`teleop-client-dev-environment.md`（v0.2） |

> 本文是"**怎么建**"的施工图：顺序、目录、批次、验收标准。
> "**为什么这么设计**"在架构文档里，本文不重复。
> 规则：**每批做完更新第 9 节，再进入下一批**；改动批次划分要写进第 10 节。

---

## 0. 一页结论

| 项 | 结论 |
|---|---|
| 施工顺序 | **先配套，后界面**：协议层 → IPC → 模拟器 → 通信进程 → 容器；Qt 与视频放到 M0/M3 |
| 批次 | **B0 ~ B5**，每批一个可独立验收的出口 |
| 工期估算 | B0 0.5 天、B1 1.5 天、B5a 0.5 天、B2 1.5 天、B3 1 天、B4 2.5 天、B5b 0.5 天（合计 5–6.5 个工作日） |
| 第一批做完的标志 | 一条命令跑通"模拟器 → 通信进程 → 模拟器"，落出 `mqtt_up.pb.zst` / `mqtt_down.pb.zst` |
| v1 明确不做 | 3D 地图、键鼠输入、多客户端、音频、云端 |

---

## 1. 施工顺序与依赖

```
B0 骨架
  └─► B1 协议层（proto + topic 表 + 编解码库）
        ├─► B5a 构建/测试容器（Dockerfile + dev-shell + CI 骨架）
        │     └─► B2 IPC 层 ──► B3 两个模拟器 ──► B4 通信进程
        │                                            └─► B5b GUI/GPU 容器 ──► M0 视频 spike ──► M3 前端
        └─► （B3 也可以只依赖 rm_proto，先跑 MQTT 侧）
```

**硬约束（每批都要守）**：

1. `teleop-node` 的依赖树里不出现 Qt / GStreamer（CMake target 依赖天然保证 + CI grep 兜底）；
2. 转发路径不改 payload 字节（`bytes` 原样进原样出）；
3. 单槽：`LatestValueRegister` 之外不允许出现无界队列；
4. 每批结束时 `ctest` 必须全绿，且 `cmake --preset release` 能编过。

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
│  └─ teleop_ipc/ ★              Apache-2.0：帧头 + SEQPACKET + 握手（header-only）
│     └─ include/teleop_ipc/{frame,seqpacket,session}.hpp
├─ src/
│  ├─ core/ ★                    时钟 / 日志 / 错误 / 配置 / LatestValueRegister / Fd RAII
│  ├─ ipc/ ★                     连 AT-Bridge：收 CustomControl、发 CustomByteBlock
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
│  ├─ teleop_sim/ ★              假 AT-Bridge（75 Hz / 50 Hz + 故障注入）
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
└─ docs/                         已有四份文档 + adr/
```

---

## 3. 构建拓扑与硬约束

```
rm_proto ────────┬──► teleop_sim
                 ├──► referee_sim
                 └──► teleop_ui（M3）

teleop_ipc ──────┬──► teleop_sim
                 └──► teleop_node

core ──┬──► ipc ──────┐
       ├──► mqtt ─────┼──► teleop_node
       ├──► record ───┤
       └──► uilink ───┘

core + uilink + rm_proto ──► teleop_ui（M3，才拉 Qt / GStreamer）
```

| 约束 | 落地方式 |
|---|---|
| 通信进程不依赖 UI 框架 | CMake target 依赖里根本没有 Qt/GStreamer；CI 再 `grep -R "Qt\|gst" src/{core,ipc,mqtt,record,node,uilink}` 兜底 |
| 协议层可被别的项目复用 | `proto/`、`libs/` 用 Apache-2.0，且不依赖 `src/` |
| 前端不能碰控制链路 | `uilink` 里不存在通往 `ipc` 的调用；代码评审 + 单测断言 |

---

## 4. 技术选择

| # | 选择 | 结论 | 理由 |
|---|---|---|---|
| 1 | proto package | `rm.terminal`（官方 32 条）、`teleop.ipc`、`teleop.ui` | 官方未给包名；只影响命名空间，不影响线格式 |
| 2 | topic 编号 | **上行 1–9、下行 10–32**（按整理稿顺序） | 本地概念，固定后录制/调试/UI 三处一致 |
| 3 | protobuf 代码 | **构建时生成**，不提交生成物 | 避免与 `.proto` 不一致 |
| 4 | 错误类型 | `tl::expected`（FetchContent） | 文档定 C++20，`std::expected` 是 C++23 |
| 5 | 第三方库 | protobuf / spdlog / GTest / Paho **用系统包**；toml++ FetchContent | 24.04 仓库都有；toml++ 是轻量头文件库 |
| 6 | MQTT | `MQTTAsync` | 75 Hz 单槽需要非阻塞发布 |
| 7 | 事件循环 | 裸 `epoll` + `eventfd` | 架构 §14.5 |
| 8 | 日志 | spdlog（async，热路径禁日志） | 架构 §14.5 |
| 9 | 代码组织 | **header-only**：一个组件 = 一个 `.hpp`，库做成 `INTERFACE` target；只有 `main.cpp`、测试文件、`protoc` 生成物是 `.cpp` | 便于阅读与跳转；避免声明/实现漂移 |

---

## 5. 命名与路径约定

| 项 | 约定 |
|---|---|
| 二进制 | `teleop-node`（通信进程）、`teleop-client`（前端）、`teleop-sim`、`referee-sim` |
| systemd unit | `teleop-node.service`（用户级） |
| socket | `~/.local/state/teleop/run/rmcs/control.sock`、`~/.local/state/teleop/run/ui.sock` |
| 配置 | `~/.config/teleop/node.toml`、`~/.config/teleop/ui.toml` |
| 录制 | `~/.local/share/teleop/sessions/<YYYYMMDD_HHMMSS>/` |
| 日志 | journald（`journalctl --user -u teleop-node`） |
| C++ 命名空间 | `teleop::core` / `teleop::ipc` / `teleop::mqtt` / `teleop::record` / `teleop::uilink`；协议代码 `rm::terminal` |
| 提交信息 | **英文**；首行首字母大写、直接用动词说清本轮改了什么（`Add ...` / `Fix ...` / `Update ...` / `Remove ...`），≤ 72 字符；不写 `feat:`/`chore:` 之类前缀；正文用短横线列小点；模板见 `.gitmessage` |
| 代码组织 | **header-only**：组件 = 单个 `.hpp`，不建 `.hpp`/`.cpp` 配对；库用 CMake `INTERFACE` target |
| 提交时机 | **不自动提交**：改动留在工作区，由用户决定何时 commit / push |

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
| 验收 | `ctest -R 'topics|codec'` 全绿 |

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
| 目标 | 两个进程能握手、能收发帧 |
| 交付物 | `libs/teleop_ipc/**`（帧头编解、socket RAII、session、握手状态机）、`tests/unit/test_frame.cpp`、`tests/unit/test_seqpacket.cpp` |
| 出口条件 | 32 字节帧头往返正确；截断帧/超长帧被拒；抽象 socket 上发一帧收一帧 |
| 验收 | `ctest -R 'frame|seqpacket'` 全绿 |

### B3 两个模拟器（1 天）

| 项 | 内容 |
|---|---|
| 目标 | 没有真实硬件也能产出数据 |
| 交付物 | `tools/teleop_sim/**`（假 AT-Bridge：75 Hz `CustomControl`、50 Hz `CustomByteBlock`、断连/慢消费者注入）、`tools/referee_sim/**`（发 23 条下行、收 8 条上行、假图传）、`scripts/sim-demo.sh` |
| 出口条件 | 两个模拟器各自能独立跑；`referee_sim` 能收到 `teleop_sim` 之外任何客户端发的上行 |
| 验收 | `./scripts/sim-demo.sh --dry-run` 打印双方收发计数 |

### B4 通信进程（2.5 天）

| 项 | 内容 |
|---|---|
| 目标 | 端到端闭环 + 落盘 |
| 交付物 | `src/core/**`、`src/ipc/**`、`src/mqtt/**`、`src/record/**`、`src/uilink/**`（server 侧）、`src/node/main.cpp`、`packaging/systemd/teleop-node.service`、`tests/integration/test_end_to_end.cpp` |
| 出口条件 | `sim-demo.sh` 起 mosquitto + 两个模拟器 + `teleop-node`，产出会话目录且含 `mqtt_up.pb.zst`、`mqtt_down.pb.zst`；单槽覆盖计数正确；`systemctl stop` 优雅退出 |
| 验收 | `ctest -R integration`；`zstd -d < mqtt_up.pb.zst | head` 能看到记录 |

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
3. 新增文件都有 SPDX 头（或已被 `REUSE.toml` 覆盖）；
4. 文档第 9 节已更新；
5. 一个提交（或一组小提交）说明清楚做了什么。

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

---

## 9. 进度记录

| 批次 | 状态 | 完成日期 | 备注 |
|---|---|---|---|
| B0 骨架 | ✅ 完成 | 2026-11-19 | 许可证 / REUSE / CMake + 五个预设 / 格式化配置 / README / 首个提交；`cmake --preset dev`、`--preset release` 均通过 |
| B1 协议层 | ✅ 完成 | 2026-11-19 | 32 条消息 + `topics.inc` + 两个内部链路 proto + `rm_proto` 编解码库（已改为 header-only）+ 23 个用例；在 Ubuntu 24.04 容器（protoc 3.21.12）里 `dev` 与 `ci`（`-Werror`）预设均全绿 |
| B5a 构建容器 | ✅ 完成 | 2026-11-19 | `docker/Dockerfile`（base/dev/ci）+ `build.sh` + `dev-shell.sh` + CI 骨架；容器内 `ci` 预 configure/build/ctest 全绿 |
| B2 IPC | ⬜ 未开始 | — | — |
| B3 模拟器 | ⬜ 未开始 | — | — |
| B4 通信进程 | ⬜ 未开始 | — | — |
| B5b GUI/GPU 容器 | ⬜ 未开始 | — | M0 之前做 |

> 状态用 ⬜ 未开始 / 🟡 进行中 / ✅ 完成 / ⛔ 受阻。每批完成后在此追加一行结论（做了什么、没做什么、遗留什么）。

---

## 10. 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-11-19 | 首次定稿：六批次划分、技术选择、命名约定、验收标准 |
