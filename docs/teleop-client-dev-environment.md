# 遥控客户端 · 开发容器设计文档

| 项 | 内容 |
|---|---|
| 文档版本 | v0.2（决策稿） |
| 日期 | 2026-11-19 |
| 状态 | 环境决策已定；**本文不包含 Dockerfile**，只定义"要什么、为什么、怎么验收" |
| 适用范围 | 本仓库的开发、单测、集成测试、CI |
| 关联文档 | `teleop-client-architecture.md`（v1.4） |

---

## 0. 一页结论

| 问题 | 结论 |
|---|---|
| 部署要不要带容器 | **不带**。产物是 `.deb` / AppImage + systemd unit，跑在裸机上 |
| 容器负责什么 | 编译一致性、单测、集成测试、CI、**GUI 开发** |
| 容器对延迟的影响 | 结构上 ≈0；真正的风险来自**配置**（CPU 配额、网络模式、驱动 ABI） |
| 开发期延迟变高 | 八成是 `-O0` 或 sanitizer 造成的，先查构建类型，再怀疑容器 |
| GUI 进容器 | 已决定，但有**三个前提**，缺一必失败（见第 4 节） |
| 容器与宿主版本不一致 | **编译/测试无影响；GUI 与硬解可能坏**，见第 12 节 |
| 发布给别人用 | 最低 **Ubuntu 24.04**；没有硬解也要能启动（软解 + 警告），见第 12 节 |

一句话：

> **容器保证"编译与测试可复现"，不保证"运行时性能"。最终延迟验收必须在真机上做一次。**

---

## 1. 目标与非目标

### 1.1 目标

| # | 目标 | 验收方式 |
|---|---|---|
| G1 | 任何装了 Docker 的 Linux 机器，克隆后 **10 分钟内**能编译并跑起 GUI | 新人照着 README 走一遍 |
| G2 | CI 与本地用**同一个镜像 digest**，避免"我这儿能过" | CI 日志打印 digest |
| G3 | 不污染宿主：不装 Qt/GStreamer、不留下 root 属主的构建产物 | `ls -l build/` 属主 = 当前用户 |
| G4 | 支持三条工作流：日常开发、集成测试、故障注入 | 见第 6 节 |

### 1.2 非目标

- ❌ 不定义**部署**形态（部署不带容器）；
- ❌ 不追求镜像体积最小（开发镜像 2–3 GB 可接受）；
- ❌ 不替代真机性能验收；
- ❌ 不支持非 Linux 宿主（仅 amd64；arm64 见第 9 节）；
- ❌ 不在容器里跑 systemd。

---

## 2. 环境基线

### 2.1 为什么是 Ubuntu 24.04

| 组件 | 24.04 提供 | 对我们的意义 |
|---|---|---|
| GStreamer | **1.24** | 满足"≥1.22"；`vah265dec`、`splitmuxsink` 齐全 |
| protobuf | **3.21** | 满足对端"≥3.15"的要求，**暂不需要 Conan** |
| Qt | **6.4** | Quick / Quick 3D / Charts 都在仓库里 |
| VA-API | libva + vainfo | 头文件齐全；**驱动用宿主的** |
| 中文支持 | `fonts-noto-cjk` | QML 界面不全变方块的前提 |

> 22.04 的问题是 GStreamer 1.20 + protobuf 3.12（低于对端要求 3.15），需要额外自管两套依赖。不划算。

### 2.2 基线一致性要求（硬性）

> **容器基线必须与宿主机发行版一致。**

原因：容器共用宿主内核的 DRM 驱动，但 userspace 的 Mesa / libva 是容器自己的。两者 ABI 不匹配时，表现为 `vainfo` 初始化失败、GL 掉到 llvmpipe、或直接段错误。

| 宿主 | 容器 | 结果 |
|---|---|---|
| 24.04 | 24.04 | ✅ 正常 |
| 22.04 | 24.04 | ⚠️ 可编译、可跑单测；GUI 与硬解**不保证** |
| 其他发行版 | 24.04 | ⚠️ 同上，需自行验证 |

如果开发机不是 24.04：要么升级，要么接受"只用于编译与单测"。

### 2.3 与主文档的三处差异（以本文为准）

| 项 | 主文档 v1.3 写的 | 本文结论 |
|---|---|---|
| Qt 版本 | 6.8 LTS | **6.4（发行版）**；`QQuickRhiItem`（6.7+）不可用，视频面走 `QQuickFramebufferObject` |
| protobuf | Conan 固定 ≥3.21 | **发行版 3.21 即可**；将来要求更高版本再引 Conan |
| 开发环境 | 无 | 本文 |

> 这三处已于主文档 **v1.3** 同步；主文档当前为 **v1.4**（协议落地后回填）。

---

## 3. 职责边界

### 3.1 容器做什么

| 场景 | 是否需要 GUI | 用途 |
|---|---|---|
| 日常编码 | ✅ | 编译 + 跑起来看效果 |
| 单元测试 | ❌ | 快速回归 |
| 集成测试 | ❌ | bridge + sim + 假前端 |
| 故障注入 | ❌ | compose 多容器，验证进程隔离与 UDS 权限 |
| CI | ❌ | 矩阵构建、sanitizer、fuzz、打 AppImage |

### 3.2 容器不做什么

- 不承载**最终性能验收**（见第 7 节）；
- 不承载**部署**；
- 不模拟 systemd（用 `tini` + 脚本即可）。

---

## 4. GUI 进容器的三个前提

缺任何一条，都会以"难查"的形式失败。

### 前提 1：容器基线与宿主发行版一致

见 2.2。这是 Mesa / libva ABI 与内核 DRM 驱动匹配的要求。

### 前提 2：容器内 uid 与宿主一致

Wayland 的 socket 位于 `XDG_RUNTIME_DIR`（`/run/user/<uid>`，权限 `0700`）。uid 不一致 → 连不上，且报错信息很不直观。

同时这条也解决了"构建产物属主变成 root"的问题。

### 前提 3：挂宿主 `/dev/dri` 与宿主 `dri` 驱动目录，**容器里不装驱动**

- 驱动（`iHD_drv_video.so`、`iris_dri.so` 等）与内核 DRM、GPU 型号强绑定；
- 装进镜像 → 换一台机器就崩；
- 正确做法：只读挂载宿主的 `/usr/lib/x86_64-linux-gnu/dri`，覆盖容器自带；
- 容器里只装 `libva2` / `vainfo` 这类**运行时与工具**，不装厂商驱动。

### 失败症状对照表

| 症状 | 最可能的原因 | 处理 |
|---|---|---|
| 启动即崩：`Failed to create OpenGL context` | 没挂 `/dev/dri`，或组权限不对 | 挂设备 + 用渲染节点的 gid 做 `--group-add` |
| `vainfo` 初始化失败 | 驱动 ABI 与内核不匹配 | 容器与宿主同发行版 |
| 能跑但极慢，渲染器显示 llvmpipe | 没挂 `/dev/dri` | 同上 |
| Wayland 连不上 | uid 不同 / `XDG_RUNTIME_DIR` 没挂 | 同 uid + 挂载运行时目录 |
| 界面文字全是方块 | 缺 CJK 字体 | 装 `fonts-noto-cjk` |
| 无显示器也能跑但窗口不出现 | 缺 `qt6-wayland` 或 QPA 插件 | 装平台插件包 |
| 偶发卡顿几百毫秒 | 设了 CPU 配额 | 去掉 `--cpus` |

---

## 5. 运行时参数要求

> 这里定义"需要哪些能力"，不是 Dockerfile。具体写法在后续交付物里。

| 能力 | 参数 | 为什么必须 |
|---|---|---|
| 网络 | **host 网络** | 避免 veth + NAT 的额外抖动；更关键的是 **netns 里 `rmem_max` 默认只有 208 KB**，直接卡死 60 fps 收包 |
| 共享内存 | `ipc=host`（或足够大的 shm） | X11 下 Qt 可能走 XShm，需要共享内存段 |
| GPU | `--device /dev/dri` | GL + VA-API |
| 组权限 | `--group-add <render 节点 gid>` | 用 gid 比用组名可靠（容器里未必有同名组） |
| 用户 | `--user <宿主 uid>:<gid>` | Wayland socket 权限；构建产物属主 |
| 显示 | `DISPLAY` / `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` 透传 | 图形输出 |
| 平台插件 | `QT_QPA_PLATFORM=wayland;xcb` | 自动适配 Wayland / X11 |
| X11 socket | 挂 `/tmp/.X11-unix` | X11 连接 |
| Wayland socket | 挂 `$XDG_RUNTIME_DIR` | Wayland 连接 |
| X11 认证 | 挂 `~/.Xauthority`（存在时，只读） | 避免 `xhost +` 这种放权做法 |
| 驱动 | 只读挂宿主 `/usr/lib/x86_64-linux-gnu/dri` | 见前提 3 |
| 源码 | 挂载 `.:/work` | 热编辑；不 COPY |
| 编译缓存 | ccache 持久化卷 | 否则每次重建都从零编译，Qt 项目尤其痛 |
| **CPU** | **不设配额** | CFS 配额耗尽会把进程 throttle 到下一个周期，最坏 100 ms 停顿 |

### 5.1 X11 与 Wayland 的差异

| | X11 | Wayland |
|---|---|---|
| 需要挂载 | `/tmp/.X11-unix` + 认证文件 | `$XDG_RUNTIME_DIR` |
| 认证 | `~/.Xauthority`（推荐）或 `xhost`（不推荐） | 靠目录权限（`0700`），所以 uid 必须一致 |
| 容器内截图/录屏 | 较麻烦 | 更难（需 portal）——但我们**只录源码流，不录屏**，无影响 |

### 5.2 WSL2 场景

若开发机是 Windows + WSL2：WSLg 会提供 `DISPLAY` / `WAYLAND_DISPLAY` 与 `/tmp/.X11-unix`。上述参数可直接沿用，但：

- `/dev/dri` 在 WSL2 中通常存在（`/dev/dri/card0`），VA-API 走 WSL 的 GPU 虚拟化，**性能与真机不同**；
- 因此 WSL2 下**只能做功能验证**，不能做延迟结论。

---

## 6. 三条工作流

| 工作流 | 形态 | 用途 | 关键点 |
|---|---|---|---|
| **日常开发** | 单容器，多进程 | 写代码、看界面 | `tini` 作 PID 1 + 一个启动脚本；bridge / ui / sim 都在同一容器 |
| **集成测试** | 单容器，无 GUI | 自动化回归 | 无头运行，`xvfb` 或直接跑非图形部分 |
| **故障注入** | compose 多容器 | 验证进程隔离与 UDS 权限 | 共享 `~/.local/state/teleop/run/rmcs`；**这才是真实的跨进程边界** |

### 6.1 为什么故障注入要用多容器

单容器里所有进程同 uid、同 mount namespace，"权限校验"形同虚设。只有拆成多个容器，才能真的验证：

- 跨容器（RMCS）与宿主之间的 UDS 是否通、uid 映射是否正确；
- 容器重启后旧 socket 残留能否被清理；
- 前端进程被 kill 时，bridge 是否真的不受影响。

### 6.2 共享 socket 目录的前置条件

从 v1.5 起 socket 放在 home 下，**不需要 root**：

```bash
install -d -m 0700 ~/.local/state/teleop/run ~/.local/state/teleop/run/rmcs
```

容器侧只挂 `rmcs/` 这一个子目录（内外路径一致），并且**以宿主 uid 运行**：

```bash
-v "$HOME/.local/state/teleop/run/rmcs:$HOME/.local/state/teleop/run/rmcs" \
--user "$(id -u):$(id -g)"
```

否则容器里的进程连不上，或者只能用 `--privileged` 糊过去——**那就测不出任何东西了**。

---

## 7. 延迟：容器到底影响什么

### 7.1 有真实影响的

| 因素 | 量级 | 说明 |
|---|---|---|
| **CPU 配额（`--cpus`）** | **最坏可达 100 ms** | 唯一"致命级"的坑；CFS quota 用完被 throttle |
| **默认 bridge 网络** | +20–100 µs，抖动↑ | veth + iptables NAT，TCP offload 可能失效 |
| **驱动 ABI 不匹配** | 硬解失效 → **+10–30 ms** | 掉到软件解码 |
| **overlayfs 写录制文件** | 首次写有 copy-up | 大文件顺序写影响小；小文件多时有感 |

### 7.2 没有影响的

| 因素 | 结论 |
|---|---|
| 容器本身（host 网络、无配额） | ≈0，测量噪声内 |
| UDS 跨容器（bind mount 共享 `~/.local/state/teleop/run/rmcs`） | ≈0，仍是同一份内核 socket |
| `ipc=host` / shm 大小 | 只决定 XShm 可用性，不改变延迟 |
| seccomp 过滤 | 每 syscall 几十纳秒，可忽略 |
| 时间戳 | 同一内核时钟，**容器内外完全可比** |

> 补充：当前 Docker 默认 seccomp 可能拦截 `io_uring` 系列 syscall。本项目用 `epoll`，不受影响；若将来换 `io_uring` 需调整安全配置。

### 7.3 调试期延迟排查顺序

延迟看起来偏高时，**按这个顺序查**，命中率从高到低：

| 顺序 | 可疑项 | 影响倍数 |
|---|---|---|
| 1 | 构建类型（`-O0 -g`） | 2–5× |
| 2 | Sanitizer（ASan / TSan） | 2–10× |
| 3 | CPU 配额 | 可能 100 ms 级停顿 |
| 4 | 网络模式（bridge 而非 host） | +几十 µs + 抖动 |
| 5 | 掉到软解（ABI 不匹配） | +10–30 ms |
| 6 | 容器本身 | ≈0 |

### 7.4 测量原则

1. **只用 Release 构建做性能结论**；sanitizer 构建只用来找 bug；
2. **容器内外可用同一套打点直接对比**——`CLOCK_MONOTONIC` 是共用的，两份直方图数字可比；
3. **最终验收在真机跑一次**：驱动、合成器、systemd 调度都只在真机上完整。

---

## 8. 环境验证清单（镜像就绪后十分钟内跑完）

| # | 检查项 | 期望 | 不满足怎么办 |
|---|---|---|---|
| 1 | `vainfo` | 出现 `driver version` 与 `HEVC` | 检查宿主是否 24.04、`dri` 是否挂上 |
| 2 | `gst-inspect-1.0 vah265dec` | 存在 | 装 `gstreamer1.0-plugins-bad`（`vah265dec` 在 `va` 插件里，包名是 **Bad Plug-ins**，不是 `gstreamer1.0-vaapi`） |
| 3 | `gst-inspect-1.0 qml6glsink` | 装 `gstreamer1.0-qt6` 后存在 | 装包；真没有才 fallback：`appsink + QQuickFramebufferObject` |
| 4 | `ls libQt6Charts*` | 存在 | 包名不对，`apt-cache search qt6-charts` |
| 5 | `ls libQt6Quick3D*` | 存在 | v1 不做 3D 地图，此项可跳过 |
| 6 | `fc-list :lang=zh` | 非空 | 装 `fonts-noto-cjk` |
| 7 | GL 渲染器（`glxinfo -B` 或 eglinfo） | 宿主 GPU，**不是 llvmpipe** | 检查 `/dev/dri` 与组权限 |

> 第 3 项基本可以关闭：`qml6glsink` 在 GStreamer Good 插件包里，Ubuntu 打包为 `gstreamer1.0-qt6`；但 AppImage 里 Qt 与 GStreamer 的 GL 上下文共享仍需实测，fallback 保留。

---

## 9. 已知风险与未决问题

| # | 项 | 现状 | 影响 |
|---|---|---|---|
| 1 | `qml6glsink` 是否可用 | **有包**（`gstreamer1.0-qt6`，GStreamer Good） | 装上即可；fallback 仍保留 |
| 2 | Qt Charts 是否打包 | 24.04 仓库有 `qt6-charts-dev` | 装上即可 |
| 3 | Quick 3D 是否打包 | 24.04 仓库有；**但 v1 不做 3D** | 不影响 v1 |
| 4 | 宿主是否统一 24.04 | **未确认**；当前开发机是 26.04 | 见 §2.2 与新增 §12 |
| 5 | 镜像体积（预计 2–3 GB） | 可接受 | 拉取时间较长，可用 registry 缓存缓解 |
| 6 | arm64 / Apple Silicon | **v0 不支持** | 只能编译 + 单测，无 VA-API |
| 7 | 是否提供 `devcontainer.json` | 待定 | 影响 VS Code / CLion 用户上手成本 |

---

## 10. 后续交付物（本文不含）

| 交付物 | 内容 |
|---|---|
| `docker/Dockerfile` | `base` / `dev` / `ci` 三阶段 |
| `scripts/dev-shell.sh` | 带全部挂载与参数的启动脚本 |
| `docker-compose.yml` | 故障注入用三服务编排 |
| `.devcontainer/devcontainer.json` | IDE 集成 |
| `.github/workflows/ci.yml` | 矩阵构建、sanitizer、fuzz、Release |
| 主文档 v1.3（已完成） | Qt 6.4、protobuf 走发行版、引用本文；主文档现已到 **v1.4** |

---

## 11. 决策记录

| # | 决策项 | 结论 | 依据 |
|---|---|---|---|
| 1 | 基线镜像 | **Ubuntu 24.04** | GStreamer 1.24、protobuf 3.21、Qt 6.4 |
| 2 | Qt 来源 | **发行版 6.4** | 避免自管 Qt；接受无 `QQuickRhiItem` |
| 3 | 容器职责 | 构建 + 单测 + 集成 + CI + GUI 开发 | 一致性优先 |
| 4 | GUI | **进容器** | 已确认；以第 4 节三个前提为准 |
| 5 | 编排 | 单容器（日常）/ compose（故障注入） | 兼顾效率与真实性 |
| 6 | 网络 | **host** | 避免 NAT 抖动与 netns 缓冲上限 |
| 7 | 用户 | **uid/gid 映射** | Wayland 权限、产物属主 |
| 8 | CPU 配额 | **不设** | 避免 100 ms 级 throttle |
| 9 | 架构 | 仅 **amd64** | 目标硬件即 x86_64 |
| 10 | 部署 | **不带容器** | 驱动、systemd、运维成本 |
| 11 | protobuf | 发行版 3.21 | 满足对端 ≥3.15 |
| 12 | 开发机 26.04 / 容器 24.04 | **容器只做编译与测试，GUI 在宿主跑** | 版本不一致时 GUI 与硬解有真实风险，见 §12 |
| 13 | 发布基线 | **最低 Ubuntu 24.04** | glibc 版本决定产物能否运行 |
| 14 | 无硬解的机器 | **允许启动**，降级软解 + 显式警告 | 使用者机器未知，不能拒绝启动 |

---

## 12. 开发机 / 容器 / 发布基线（v0.2 新增）

### 12.1 现状

| 项 | 版本 |
|---|---|
| 开发机（宿主） | Ubuntu **26.04**，Qt 6.10.2，GStreamer 1.28.2，Mesa 26.x |
| 计划容器 | Ubuntu **24.04**，Qt 6.4，GStreamer 1.24，Mesa 24.x |
| 发布基线 | **最低 Ubuntu 24.04**；其余发行版"能跑不保证" |

### 12.2 版本不一致到底坏在哪（按严重度）

1. **编译与单测：无影响。**
2. **图形（OpenGL）：可能悄悄变慢。** 容器里的 Mesa 不认识宿主核显时会退回软件渲染（llvmpipe），窗口照样开、不报错，只是卡。当前 13 代核显在 24.04 的 Mesa 支持范围内，大概率没事。
3. **硬解（VA-API）：最容易坏，而且不会自动降级。** 容器里的 libva、`iHD_drv_video.so`、宿主内核必须配套。**"挂宿主 dri 驱动"和"容器与宿主同版本"是绑定的**：版本不一致时宿主的驱动比容器 libva 新，加载失败 → `vainfo` 报错 → 没有硬解。
4. **Wayland**：老的 libwayland 连新合成器一般没事；坏的多是缺 `$XDG_RUNTIME_DIR` / dbus / portal。
5. **X11**：基本无影响。
6. **网络**：必须 `--network=host`，否则收不到 `192.168.12.1` 发往 `192.168.12.2:3334` 的图传。
7. **CPU 配额**：`--cpus` 会产生几十毫秒级节流，对 60 fps 致命。

### 12.3 因此怎么用容器

- **容器只做编译 / 单测 / 集成 / CI**：没有版本不一致的问题。
- **要跑 GUI + 硬解**：把镜像换成与宿主一致的 26.04，或者干脆在宿主上跑（推荐，最省事）。
- **出包用 24.04**：glibc 版本决定产物能跑在哪些机器上——在 26.04 上编出来的二进制，到 24.04 / 22.04 上会直接起不来（`GLIBC_2.xx not found`）。

### 12.4 10 分钟自检（在宿主上跑）

```bash
docker run --rm -it --network=host \
  --device /dev/dri --group-add "$(stat -c '%g' /dev/dri/renderD128)" \
  -e DISPLAY -e WAYLAND_DISPLAY -e XDG_RUNTIME_DIR \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v /usr/lib/x86_64-linux-gnu/dri:/usr/lib/x86_64-linux-gnu/dri:ro \
  ubuntu:24.04 bash -c 'apt update && apt install -y vainfo mesa-utils && vainfo && glxinfo -B | head -20'
```

把 `-v .../dri:...` 那行去掉再跑一次，两次对比：

- 两次 `vainfo` 都通过、渲染器都不是 `llvmpipe` → 容器可以直接用；
- 只有挂宿主驱动那次通过 → ABI 不匹配，**不要挂**，或者把镜像换成 26.04。

### 12.5 发布时的兼容性约定

| 层 | 约定 |
|---|---|
| glibc | 打包机用**最老的受支持发行版**（当前 = 24.04） |
| GStreamer | **不打包**，依赖系统（≥1.22）；README 写明所需包 |
| VA-API 驱动 | **不打包**；README 写明各发行版包名（Ubuntu/Debian `intel-media-va-driver`、Fedora RPM Fusion、Arch `intel-media-driver`） |
| Qt | AppImage 内打包 Qt 与平台插件（`libqxcb.so` + `libqwayland*.so`） |
| 硬解缺失 | **允许启动**，降级软解 + 显式警告（1080p60 可能卡） |
| 承诺范围 | Ubuntu 24.04+ / Fedora / Arch；KDE / GNOME / XFCE；X11 与 Wayland |
