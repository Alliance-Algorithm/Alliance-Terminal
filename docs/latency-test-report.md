# RMCS → Alliance-Terminal 延迟测试报告

> 日期：2026-11-22
> 范围：RMCS 终端桥 `Link::send_up` 到客户端 `ipc-probe` 收到 `UpData`
> 结论先行：**当前没有 CPU 隔离时，两边都不绑核的综合尾部最好；绑核能降低 p50，但显著放大 p99/p99.9。**

## 1. 结论摘要

60 B 数据、600 s、不绑核基线：

```text
samples  44925
p50      0.137 ms
p95      0.219 ms
p99      0.282 ms
p99.9    0.452 ms
max      3.42 ms
>1 ms    12 / 44925
```

120 s 的短基线结果与它一致：p50 0.141 ms、p99 0.287 ms、p99.9 0.405 ms。

绑核后：

- p50 最低到 **0.095 ms**，比 baseline 好约 **46 µs**；
- 但 p99 升到 **1.0–1.4 ms**；
- p99.9 升到 **约 3.0 ms**；
- `>1 ms` 样本从 **12 个 / 600 s** 变成 **92–121 个 / 120 s**。

因此当前建议：

- **正式基准和后续测试暂时采用“不绑核”**；
- 不再把 probe 绑到 CPU 6；
- 如果以后要继续降低 p50，必须做真正的 CPU 隔离（`cset shield` / `isolcpus`），不能只用 `taskset`；
- 600 s 不绑核复测已完成，p99.9 0.452 ms，基线确认。

## 2. 测量口径

### 2.1 当前指标是什么

当前代码里的端到端延迟：

```text
t_src_ns  = 桥 executor 本轮 scheduled_start
t_recv_ns = probe poll(fd) 返回后、处理 UpData 前的时间
latency   = t_recv_ns - t_src_ns
```

时间线：

```text
executor scheduled_start
  → executor 实际醒来 / start_late
  → 前序组件 update
  → TerminalBridgeTx update
  → encode_custom_control
  → encode_up_data / session prepare
  → sendmsg
  → UDS
  → probe poll 唤醒
  → t_recv_ns
  → recv / 解码
```

它包含：

- bridge executor 调度和 start_late；
- 组件执行顺序；
- 桥侧编码、组帧、发送；
- UDS 传输；
- 客户端 poll 唤醒。

它不包含：

- RMCS 作者产生 `CustomControlData` 的时间；
- 客户端收到后交给 MQTT/录制的处理时间。

所以严格说是 **“桥 executor scheduled_start → probe poll 唤醒”**，不是 wire `sendmsg → recvmsg`，也不是完整业务端到端。当前阶段按“当前链路端到端”使用。

### 2.2 数据段大小

所有 01–06 记录都是：

```text
payload_size = 62
数据段       = 60 B
```

`payload_size` 是官方 `CustomControl` envelope 大小，62 B = `0A 3C` + 60 B 数据。

## 3. 测试工具

客户端：

- `ipc-probe --latency`：聚合 p50/p95/p99/max；
- `ipc-probe --record PATH`：每帧 CSV + 自动 metadata JSON；
- `ipc-probe --rtt`：Echo token 回环（本轮不作为验收重点）；
- `tools/plot_latency.py`：时间序列、直方图、每秒 p50/p95/p99。

示例：

```bash
./build/dev/tools/ipc_probe/ipc-probe \
  --socket ~/.local/state/teleop/run/rmcs/control.sock \
  --peer-uid -1 \
  --duration 120 \
  --record .records/latency01/latency.csv

python3 tools/plot_latency.py .records/latency01/latency.csv
```

CSV 主要字段：

```text
sample,seq,t_src_ns,t_recv_ns,latency_ns,payload_size,state,peer_uid,warmup
```

metadata 包含 socket、方向、协议版本、样本数、warmup 数、p50/p95/p99/max 和最后错误。

## 4. 测试环境

| 项 | 内容 |
|---|---|
| 宿主 | Ubuntu 26.04，Linux 7.0 |
| CPU | i5-13420H：4 个 P-core（CPU 0–7，含 HT）+ 4 个 E-core（CPU 8–11） |
| NUMA | 单节点 |
| RMCS | `qzhhhi/rmcs-develop:latest-full` 容器 |
| 客户端 | 宿主 `ipc-probe` |
| socket | `~/.local/state/teleop/run/rmcs/control.sock`，bind mount 进容器 |
| 容器 CPU 限制 | 无 cpuset、无 quota，默认 0–11 |
| 执行器 | 1 kHz `rmcs_executor` |
| 上行 | 75 Hz `UpData`，60 B 数据 |
| 下行 | 50 Hz `DownData` |

未开启：

- `isolcpus` / `nohz_full` / `rcu_nocbs`；
- `cset shield`；
- 实时调度 `SCHED_FIFO`；
- 容器 cpuset 独占。

## 5. 测试矩阵与结果

00 为 600 s；01–06 为 120 s；全部 60 B 数据。

| 编号 | 配置 | p50 | p95 | p99 | p99.9 | max | >1 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| 00 | 两边都不绑，600 s | **0.137 ms** | **0.219 ms** | **0.282 ms** | **0.452 ms** | 3.42 ms | **12 / 44925** |
| 01 | 两边都不绑，120 s | 0.141 ms | 0.220 ms | **0.287 ms** | **0.405 ms** | 3.35 ms | **4 / 8925** |
| 02 | 桥 CPU 4 + probe CPU 6，`ros2 launch` | 0.095 ms | 0.220 ms | 1.401 ms | 2.971 ms | 3.93 ms | 121 |
| 03 | 桥 CPU 4 + probe CPU 6，`ros2 run` | 0.096 ms | 0.209 ms | 1.002 ms | 3.038 ms | 4.01 ms | 92 |
| 04 | 只绑 probe CPU 6 | 0.128 ms | 0.246 ms | 0.477 ms | 2.293 ms | **11.39 ms** | 54 |
| 05 | 只绑桥 CPU 4 | 0.114 ms | 0.222 ms | 0.320 ms | 2.705 ms | 3.65 ms | 25 |
| 06 | probe CPU 6 + 桥 CPU 8（E-core） | 0.117 ms | 0.239 ms | 1.122 ms | 3.143 ms | 4.00 ms | 111 |
| 无 record | 桥 CPU 4 + probe CPU 6 | 0.095 ms | 0.218 ms | 1.044 ms | — | 4.17 ms | — |

### 5.1 图表

本地图表在：

```text
.records/latency01/latency_*.png
.records/latency02/pinned-bridge4-probe6_*.png
.records/latency03/pinned-bridge4-probe6_*.png
...
```

`.records/` 在 `.gitignore` 中，只保留在本地。

## 6. 分析

### 6.1 不绑核：p50 略高，但尾部最好；600 s 已确认

00（600 s）：

- p50 0.137 ms；
- p99 0.282 ms；
- p99.9 0.452 ms；
- `>1 ms` 12 / 44925（0.027%）；
- 每秒出现 `>0.8 ms` 的尖峰只有 9 次（600 s 内）。

01（120 s）与它一致，说明基线稳定。这是目前最稳定的配置。

### 6.2 两边都绑：p50 最好，尾部明显变差

02/03：

- p50 约 0.095 ms，比 baseline 低约 46 µs；
- p99 1.0–1.4 ms；
- p99.9 约 3.0 ms；
- `>1 ms` 92–121 个。

**中位数改善不值得用 3–4 倍 p99 恶化来换。**

### 6.3 CPU 6 是主要问题

- 04 只把 probe 绑到 CPU 6：p99 0.477 ms，max 11.39 ms；
- 06 probe 在 CPU 6、桥放在 E-core 8：p99 1.122 ms，尾部仍然差；
- 05 不绑 probe、只把桥绑到 CPU 4：p99 0.320 ms，明显好于 04/02/03。

所以当前数据下：

> **probe 绑 CPU 6 是尾部恶化的主要来源，而不是“绑核”本身。**

CPU 6/7 是同一个 P-core 的超线程；CPU 7 上的其他任务、IRQ 或内核线程都可能和 probe 抢执行资源。`taskset` 只限制亲和性，不隔离其他任务。

### 6.4 不是 CSV 录制

无 `--record` 的结果：

```text
p50 0.095 ms
p99 1.044 ms
max 4.17 ms
```

与 03 基本一致，录制 I/O 不是主因。

### 6.5 不是 `ros2 launch`

02（launch）和 03（run）结果接近，launch 包装层不是主因。

### 6.6 只绑桥是折中，但不优于不绑

05：

```text
p50 0.114 ms，p99 0.320 ms
```

p50 略好，但 p99.9 从 0.405 退到 2.705 ms，`>1 ms` 从 4 变 25。  
综合仍不如 01。

## 7. 当前建议

1. **正式基准用不绑核**；
2. 不再使用 probe → CPU 6；
3. 600 s 不绑核基线已完成（00），后续正式对比使用 00；
4. 如果要继续优化 p50，先做 CPU 隔离：
   - `cset shield`
   - `isolcpus=... nohz_full=... rcu_nocbs=...`
   - 然后再绑 executor/probe；
5. 不要用 `docker update --cpuset-cpus` 把整个 RMCS 容器挤到一个核，DDS/launch/executor 会互相竞争；
6. 每次实验记录：
   - 桥/probe 的 `Cpus_allowed_list`；
   - 是否 `--record`；
   - payload 大小；
   - 120/600 s；
   - 1 分钟 p99 和 max。

## 8. 局限与后续

- 600 s 基线已完成（00）；120 s 的短测用于快速对比；
- 当前指标包含 executor 调度，不能和 wire 延迟混用；
- 当前不含作者产生时间和 MQTT publish；
- 如果以后需要 wire 延迟，可以用 `Session::prepare` 的 `frame.header.t_mono_ns` + 客户端 `recv` 返回时间，不需要改 IPC 协议；
- 如果以后需要完整业务端到端，需要作者 source timestamp 和 MQTT publish timestamp。

## 9. 记录位置

```text
.records/latency00/   不绑核 baseline，60 B，600 s
.records/latency01/   不绑核 baseline，60 B，120 s
.records/latency02/   桥4 + probe6，ros2 launch，120 s
.records/latency03/   桥4 + probe6，ros2 run，120 s
.records/latency04/   probe6 only，120 s
.records/latency05/   bridge4 only，120 s
.records/latency06/   probe6 + bridge8，120 s
```

每个目录包含：

```text
*.csv
*.csv.meta.json
*_summary.txt
*_timeseries.png
*_histogram.png
*_percentiles.png
```
