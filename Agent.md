# Agent.md · 给 AI 助手的工作约定

> 本仓库的 AI 助手行为规范。每次接手任务先读它，再读设计文档。
> 规则和文档冲突时以本文为准；本文没写的，去问人。

---

## 1. 项目一句话

RoboMaster 自定义终端的操作端客户端：两个进程（通信进程 + 前端），接 RMCS、接裁判系统 MQTT、收 HEVC 图传、录制回放。

## 2. 必读文档（顺序）

| 顺序 | 文档 | 看什么 |
|---|---|---|
| 1 | `docs/teleop-client-architecture.md` | 架构与决策，**改动前必看** |
| 2 | `docs/custom-terminal-protocol.md` | 官方 32 个 topic 的字段与坑 |
| 3 | `docs/teleop-client-implementation-plan.md` | 当前批次、目录、验收标准 |
| 4 | `docs/teleop-client-dev-environment.md` | 环境基线、容器参数、发布兼容性 |
| 5 | `docs/ipc-protocol.md` | IPC 线格式、payload 边界、终端桥契约、rmcs-runtime 部署（**共用文档**，桥仓有同内容副本） |

---

## 3. 硬性规则（违反即返工）

### 3.1 不许自动提交

- **永远不要自己 `git commit` / `git push`**，除非用户明确说"提交"。
- 做完一段工作只汇报，改动留在工作区，由用户决定何时提交。
- 用户说"撤回 commit"时用 `git reset --mixed HEAD~1`（保留文件），不要 `--hard`。

### 3.2 提交信息格式

```
Add client framework

- Split bridge and UI into two processes
- Add UDS framing layer with 32-byte header
```

- 英文；首行首字母大写、动词开头（Add / Fix / Update / Remove / Refactor / Document）
- 一行 ≤ 72 字符；**不写** `feat:` / `fix:` / `chore:` 前缀
- 正文用短横线列小点，一条一行
- 模板见 `.gitmessage`

### 3.3 代码是 header-only

- **一个组件 = 一个 `.hpp`**，声明和实现同文件，函数 `inline`。
- 不创建成对的 `.hpp` + `.cpp`。
- 只有三类文件可以是 `.cpp`：可执行入口 `main.cpp`、测试文件、`protoc` 生成的 `.pb.cc`。
- 库一律 CMake `INTERFACE` target；头文件里的全局对象必须 `inline`。

### 3.4 注释能省则省

- **只写"为什么"和"约束"**：单位、上限、线程安全、所有权、与官方文档不同之处。
- 不写"是什么"（`// 设置 x 为 1`、`// 循环 32 次`、`// 返回结果` 这类一律不要）。
- 公共头文件的函数契约写一句（失败返回什么、是否线程安全）。
- 不留注释掉的代码。

### 3.5 先问再改

- 改协议、改架构、加依赖、换目录结构之前**先问**。
- 拿不准就问，不要猜；猜了要在回复里标出"这是假设"。

### 3.6 每批完成要跟进

- 更新施工计划第 9 节：做了什么、没做什么、遗留什么。
- 汇报顺序：结论 → 验证结果（命令 + 输出）→ 设计取舍 → 下一步。

### 3.7 一律走分支 + PR（不许直接推 main）

`main` 受保护，**永远不要直接 `git push origin main`**，也不要直接在主分支上提交。

流程：

1. 从最新的 `main` 开分支，名字用 `feat/…`、`fix/…`、`docs/…`、`chore/…`（英文、小写、短横线）；
2. 在分支上完成功能与测试；
3. 推送分支，开 PR 到 `main`；
4. PR 必须两个预设都绿（`dev` 与 `ci`），描述写清：改了什么、怎么验证、有什么取舍；
5. 合并用 **squash**，PR 标题就是最终的提交信息（英文、首字母大写、动词开头、正文列小点）；
6. 合并后删掉分支。

例外只有一种：用户明确说"直接推到 main"。即便如此，也要先确认 CI 能过。

### 3.8 文档归属与共用文档

- **任何文档不得直接放在 `~/Workspace/Alliance/` 根目录**；只放在所属仓的 `docs/` 或仓内约定位置。
- 只属于客户端的文档放本仓；只属于 `rmcs_terminal_bridge` 的放桥仓；**两边都要用的文档两边各一份，内容必须一致**。
- 当前共用文档（两份）：
  - `Alliance-Terminal/docs/ipc-protocol.md`
  - `rmcs_terminal_bridge/docs/ipc-protocol.md`
- 共用文档头部必须写：**对端路径、版本、最后同步日期、正文 SHA256、修改提醒**。
- 修改共用文档时**必须同时修改对端**，并更新两边头部的版本/日期/指纹；只改一边视为未完成。
- 正文一致性检查（两边结果应等于头部 `正文指纹`）：

  ```bash
  sed -n '/^## 0\./,$p' docs/ipc-protocol.md | sha256sum
  ```

- 桥仓自己的文档规则写在它自己的 `docs/design.md` 顶部或 `AGENTS.md`；两边规则冲突时，以所在仓的规则为准。

---

## 4. 数据流铁律

1. **只允许通信进程碰 RMCS 和 MQTT**；前端两者都不碰。
2. **转发路径上一个字节都不许改**：`CustomControl` / `CustomByteBlock` 原样进出。
3. **不许自造 topic**：官方只有 32 个，多一个都不发。
4. **单槽语义**：状态类覆盖，触发类走有界缓冲；不许出现无界队列。
5. **丢弃必须计数**，计数要能被 UI 和录制看到。
6. **前端只能发 7 条指令**：`MapClickCmd`、`AssemblyCommand`、`RobotPerformanceSelectionCommand`、`CommonCommand`、`RuneActivateCommand`、`DartCommand`、`SentryCtrlCommand`；其余通道一律拒绝。
7. **回放只读文件**，回放期间禁用发指令。
8. **客户端不解 payload**：`UpData.payload` / `DownData.payload` 是官方 `CustomControl` / `CustomByteBlock` 的序列化字节；只有终端桥 `rmcs_terminal_bridge` 组/解，客户端只搬运。`libs/teleop_ipc` 不包含这两个官方消息的 codec。

> `rmcs_terminal_bridge` 是 RMCS 仓里的独立子模块（GPL-3.0-only），遵循 RMCS 的构建/编码规范；**本文件的 header-only、`.hpp` 单文件等规则不适用于它**。它消费 `libs/teleop_ipc`，线格式契约见 `docs/ipc-protocol.md`。

---

## 5. 代码风格

### 5.1 命名（与 RMCS 一致）

| 对象 | 规则 | 例子 |
|---|---|---|
| 类型 / 类 / 结构体 | `PascalCase` | `TopicDef`、`LatestValueRegister` |
| 函数 / 方法 / 变量 | `snake_case` | `decode()`、`t_recv_ns` |
| 类成员 | `snake_case` + 尾下划线 | `logger_`、`queue_` |
| 常量 / `constexpr` | `k` + `PascalCase` | `kTopicCount`、`kMaxFrameBytes` |
| 命名空间 | 小写 | `teleop::core`、`rm::proto` |
| 枚举值 | `PascalCase`（用 scoped enum） | `Direction::Up` |
| 文件名 | `snake_case`，单数 | `codec.hpp` |
| 宏 | 全大写；能不用就不用（X-macro 例外） | `X(...)` |

### 5.2 单位与类型

- **单位进名字**：`t_recv_ns`、`timeout_ms`、`rate_hz`、`len_bytes`、`load_pct`；不许出现裸 `timeout`、`latency`。
- 协议字段一律定宽类型（`std::uint32_t` 等），不用 `int`。
- 大小用 `std::size_t`；长度参与校验时先转成有符号或显式比较，避免无符号回绕。
- 禁止隐式窄化；用花括号初始化（`int x{...}`）。

### 5.3 资源与错误

- 只用 RAII；禁止裸 `new` / `delete`；fd 用 `Fd` 封装；线程用 `std::jthread` 或受管包装。
- **不用异常**，错误一律 `tl::expected<T, Error>`。
  例外：`src/ui`、`src/app` 按 Qt 的规矩来（Qt 自己会抛），`main()` 顶层可以兜一个 `catch`。
- 禁止 C 风格转换；`reinterpret_cast` 只允许出现在协议 / UDP / dmabuf 边界，且必须写一句理由。

### 5.4 头文件

- `#pragma once`。
- 头文件里禁止 `using namespace`；单参数构造 `explicit`。
- 头文件里的函数、变量一律 `inline`（或 `inline constexpr`）。
- 包含顺序：标准库 → 第三方 → 本项目；每组内按字母序。

### 5.5 例外

Qt 相关目录（`src/ui`、`src/app`）允许使用 Qt 的异常、信号槽和定时器；但**跨进程协议、数据流和时钟**仍然走本文的规则。

---

## 6. 架构约束

| # | 约束 |
|---|---|
| 1 | **依赖单向**，由 CMake target 强制：`core ← ipc/mqtt/record/uilink ← node`；`teleop_node` 的链接闭包里出现 Qt / GStreamer 即违规（CI 再 `grep` 兜底） |
| 2 | **线程间交接只有两种**：单槽（最新值）或有界队列；无界队列禁止；每个丢弃点必须有计数器 |
| 3 | **外部输入先验长度再解析**：IPC 帧 ≤ 64 KB、UDP 包 ≤ 1400 B、单帧 ≤ 4 MB；未知 topic / 未知字段忽略并计数，不许崩 |
| 4 | **时钟统一** `core::Clock`；业务代码不许直接调 `clock_gettime`（回放和测试要能注入假时钟）。`CLOCK_REALTIME` 只用于墙钟映射 |
| 5 | **配置启动读一次**；运行期只允许改日志级别和 UI 主题 |
| 6 | **一个进程一个事件循环**；线程在启动阶段创建完，运行期不再新增；禁止 `detach` |
| 7 | **实盘与回放共用 `DataSource`**；加数据源不改上层 |
| 8 | **存储格式版本化**：`manifest.json` 带 `schema_version`，新版本必须能读旧会话（B4 落地） |

---

## 7. 测试与验收

| # | 约束 |
|---|---|
| 1 | 协议消息必须有 **golden bytes**（编码字节逐个比对）和**回环**（解码再编码）测试 |
| 2 | 依赖时间的逻辑必须能注入假时钟；**测试里禁止 `sleep`** |
| 3 | 每批结束 `dev` 与 `ci` 两个预设都要绿；`ci` 带 `-Werror` |
| 4 | 性能与延迟结论只在 `release` 预设下得出 |
| 5 | 新依赖必须写进施工计划 §4 技术选择表，并注明许可证 |

---

## 8. 构建与验证

**首选容器**（Ubuntu 24.04，和 CI 同一个镜像；容器内是 GCC 13 / CMake 3.28 / protobuf 3.21）：

```bash
./docker/build.sh dev        # 构建开发镜像（一次）
./docker/dev-shell.sh        # 进容器交互

./docker/dev-shell.sh -- bash -lc 'cmake --preset ci && cmake --build --preset ci -j"$(nproc)" && ctest --preset ci'
```

VS Code 用 `.devcontainer/devcontainer.json` 直接进容器（容器内用户 `ubuntu`，uid 与宿主一致）。
容器默认 shell 是 zsh + oh-my-zsh；构建时 oh-my-zsh 官方仓库不通会自动退到 gitee 镜像，可用 `--build-arg OMZ_REPOS="url1 url2"` 覆盖。

**不要在同一个 `build/<preset>` 里混用宿主与容器**：编译器一变，CMake 会拒绝复用缓存；删掉该目录重配即可。

**宿主直编**（自己装好依赖时也行）：

```bash
sudo apt install cmake ninja-build g++ protobuf-compiler libprotobuf-dev libgtest-dev
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

预设：`dev`（Debug）、`release`（性能结论只认它）、`ci`（Release + `-Werror`）。

**容器必须同路径挂载**（`dev-shell.sh` 已经这么做）：否则 `compile_commands.json` 里是容器路径，
编辑器找不到头文件。容器以宿主 uid 运行，`build/` 不会变成 root 所有。

编辑器入口：

```bash
ln -sfn build/dev/compile_commands.json compile_commands.json
```

（`.clangd` 里也写了一份兜底配置，指向 `build/dev`。）

**公共 include 根要挂在顶层 `include_directories()`**：clangd 给头文件推断编译命令时会借用某条 TU 的命令，
如果那条 TU 是生成代码（只有 `-isystem .../generated`），头文件就解析不到。新增库时把它的 include 根一并加进去。

---

## 9. 版本基线

| 组件 | 版本 |
|---|---|
| 发行版 | Ubuntu 24.04（最低支持） |
| Qt | 6.4（发行版；没有 `QQuickRhiItem`） |
| GStreamer | 1.24（≥1.22 即可） |
| protobuf | 3.21 |
| 编译器 | GCC 13+ / Clang 16+ |

---

## 10. 沟通方式

- 用中文回复，**先说结论**，再说细节。
- 长讨论给表格或清单，别写成散文。
- 不确定就问，不要为了显得能干而猜。
- 每轮结束给"下一步"，但不要擅自开始下一批。
