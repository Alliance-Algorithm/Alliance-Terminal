# 自定义终端协议 · 指令手册

| 项 | 内容 |
|---|---|
| 整理对象 | 《RoboMaster 2027 机甲大师高校系列赛通信协议 V1.0.0（20261029）》第 2 章「自定义终端协议」 |
| 覆盖范围 | 2.1 指令概览 + 2.2 详细协议定义，共 **32 个 topic**（发送 9 个 / 接收 23 个） |
| 整理日期 | 2026-11-19（v2：补认证/方向约定、修 5 处原文问题、加实现要点 11–13） |
| 整理原则 | 只做搬运与重排，不改写协议语义；字段编号 = protobuf 的 tag 编号 |

> 文中「服务器」指裁判系统服务器（`192.168.12.1`），「自定义终端」指本节协议的操作端（`192.168.12.2`）。
> 文中标注 `【原文注】` 的地方是原文表述存在歧义或自相矛盾之处，实现前建议向官方确认。

---

## 0. 传输层约定

| 项 | 规定 |
|---|---|
| 序列化 | Protobuf v3（Protocol Buffers），支持 3.15 及以上版本 |
| 传输 | MQTT（发布/订阅），**直接传输 protobuf 序列化后的裸二进制流**，不加 envelope |
| topic 命名 | topic 即「指令名」，与本手册各节标题（即消息名）完全一致 |
| 服务器地址 | 固定 IP `192.168.12.1`，端口 `3333` |
| 自定义终端地址 | `192.168.12.2` |
| clientID | `MyMqttClient` 的 clientID 需填**所连接机器人对应的 ID 编号**（ID 见附录 B） |
| 认证 | **无**用户名/密码/TLS（原文未定义），clientID 正确即可连接 |
| 方向约定 | **上行** = 自定义终端 → 服务器/机器人；**下行** = 服务器/机器人 → 自定义终端 |
| 最高 QoS | 全部 32 个 topic 均为 **1** |
| 图传码流 | 另开 UDP 监听 `3334` 端口，60 fps，1920×1080，HEVC（标准 H.265），**无重传机制** |

通信流程（原文）：

1. 结合文档中的 Protobuf 消息格式编写 `.proto` 文件，用 `protoc` 生成不同语言的类代码；
2. 在发布者端，将对象序列化成二进制；
3. 通过 MQTT publish 把二进制发送到对应 topic；
4. 在订阅者端，接收消息并用 Protobuf 反序列化。

图传码流 UDP 包格式：每个包前 8 个字节固定为「帧编号（递增）2 B + 当前帧内分片序号 2 B + 当前帧总字节数 4 B」，**该 8 字节为大端传输**；单包最大 1400 字节，去掉 8 字节头后有效载荷 1392 字节。

> **包头里没有时间戳**（原文如此）。接收端只能自己造时间轴：显示用"到达即渲染"，录制用"帧号 × 1/60 秒"外加分段校准。

### 与图传链路命令字的对应关系

| 链路命令字 | 载荷上限 | 方向 | 对应 topic |
|---|---|---|---|
| `0x0311` | 60 字节 | 自定义终端 → 图传链路 → 机器人 | `CustomControl`（原文明确）；`KeyboardMouseControl` 亦经图传链路下发 |
| `0x0310` | 300 字节 | 机器人 → 图传链路 → 自定义终端 | `CustomByteBlock` |
| `0x0303` | — | 选手端 → 机器人（小地图交互） | 与 `MapClickInfo` / `MapClickCmd` 的频率限制相关（见 1.3.1） |

> **【原文注】** 附录三示例代码中 `PublishMsg` / `PublishBytesMsg` 的默认 QoS 参数为 `ExactlyOnce`（即 QoS 2），与 2.1 概览「最高 qos 等级 = 1」不一致。按概览表，实际发送应压到 QoS ≤ 1。

---

## 1. 速查表

### 1.1 发送组（自定义终端 → 服务器 / 图传链路）

| topic_name | 发送路径 | 最高 QoS | 频率 | 用途 |
|---|---|---|---|---|
| `KeyboardMouseControl` | 自定义终端 → 图传链路 → 机器人 | 1 | 75Hz | 传输鼠标键盘输入 |
| `CustomControl` | 自定义终端 → 图传链路 → 机器人 | 1 | 75Hz | 最大 60 字节的自定义数据 |
| `MapClickCmd` | 自定义终端 → 服务器 | 1 | 触发式，频率限制同 1.3.1 | 地图点击标记指令 |
| `AssemblyCommand` | 自定义终端 → 服务器 | 1 | 触发式，最高 10Hz | 重装装配指令 |
| `RobotPerformanceSelectionCommand` | 自定义终端 → 服务器 | 1 | 触发式，最高 10Hz | 地面机器人选择性能体系或控制方式 |
| `CommonCommand` | 自定义终端 → 服务器 | 1 | 触发式，最高 10Hz | 机器人多种常用指令 |
| `RuneActivateCommand` | 自定义终端 → 服务器 | 1 | 触发式，最高 10Hz | 能量机关激活指令 |
| `DartCommand` | 自定义终端 → 服务器 | 1 | 触发式，最高 10Hz | 飞镖控制指令 |
| `SentryCtrlCommand` | 自定义终端 → 服务器 | 1 | 触发式，最高 10Hz | 哨兵控制指令请求 |

### 1.2 接收组（服务器 / 图传链路 → 自定义终端）

| topic_name | 接收路径 | 最高 QoS | 频率 | 用途 |
|---|---|---|---|---|
| `GameStatus` | 服务器 → 自定义终端 | 1 | 5Hz | 同步比赛全局状态信息 |
| `GlobalUnitStatus` | 服务器 → 自定义终端 | 1 | 1Hz | 同步全局胜负相关状态 |
| `GlobalLogisticsStatus` | 服务器 → 自定义终端 | 1 | 1Hz | 同步全局后勤信息 |
| `GlobalSpecialMechanism` | 服务器 → 自定义终端 | 1 | 1Hz | 同步正在生效的全局特殊机制 |
| `Event` | 服务器 → 自定义终端 | 1 | 触发式 | 全局事件通知 |
| `RobotInjuryStat` | 服务器 → 自定义终端 | 1 | 1Hz | 机器人一次存活期间累计受伤统计 |
| `RobotRespawnStatus` | 服务器 → 自定义终端 | 1 | 1Hz | 机器人复活状态同步 |
| `RobotStaticStatus` | 服务器 → 自定义终端 | 1 | 1Hz | 机器人固定属性和配置 |
| `RobotDynamicStatus` | 服务器 → 自定义终端 | 1 | 10Hz | 机器人实时数据 |
| `RobotModuleStatus` | 服务器 → 自定义终端 | 1 | 1Hz | 机器人各模块运行状态 |
| `RobotPosition` | 服务器 → 自定义终端 | 1 | 1Hz | 机器人空间坐标和朝向 |
| `Buff` | 服务器 → 自定义终端 | 1 | 获得增益时触发，此后 1Hz 定频直到失去增益 | Buff 效果信息 |
| `PenaltyInfo` | 服务器 → 自定义终端 | 1 | 触发式 | 判罚信息同步 |
| `RobotPathPlanInfo` | 服务器 → 自定义终端 | 1 | 触发式 | 哨兵轨迹规划信息 |
| `MapClickInfo` | 服务器 → 自定义终端 | 1 | 触发式，频率限制同 1.3.1 | 地图点击标记信息同步 |
| `RadarInfoToClient` | 服务器 → 自定义终端 | 1 | 触发式 | 雷达发送的机器人位置信息 |
| `CustomByteBlock` | 机器人 → 图传链路 → 自定义终端 | 1 | 50Hz | 机器人自定义上传数据流（`0x0310`） |
| `TechCoreMotionStateSync` | 服务器 → 自定义终端 | 1 | 1Hz | 科技核心运动状态同步 |
| `RobotPerformanceSelectionSync` | 服务器 → 自定义终端 | 1 | 1Hz | 步兵/重装性能体系状态同步 |
| `RuneStatusSync` | 服务器 → 自定义终端 | 1 | 1Hz | 能量机关状态同步 |
| `SentryStatusSync` | 服务器 → 自定义终端 | 1 | 1Hz | 哨兵姿态相关信息同步 |
| `DartSelectTargetStatusSync` | 服务器 → 自定义终端 | 1 | 1Hz | 飞镖目标选择状态同步 |
| `SentryCtrlResult` | 服务器 → 自定义终端 | 1 | 触发式 | 哨兵控制指令结果反馈 |

---

## 2. 发送组 · 详细定义

### KeyboardMouseControl

| 项 | 值 |
|---|---|
| topic_name | `KeyboardMouseControl` |
| 方向 | 自定义终端 → 图传链路 → 机器人 |
| 最高 QoS | 1 |
| 频率 | 75Hz |
| 用途 | 传输鼠标键盘输入 |

该数据将通过相机图传模块（接收端）发送给机器人。实际比赛时，操作手需提前选择键鼠控制指令来源为官方选手端或自定义终端，**同时仅能通过一个来源**生成发送给机器人的键鼠指令。

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | int32 | `mouse_x` | 鼠标 x 轴移动速度，负值表示向左移动 |
| 2 | int32 | `mouse_y` | 鼠标 y 轴移动速度，负值表示向下移动 |
| 3 | int32 | `mouse_z` | 鼠标滚轮移动速度，负值表示向后滚动 |
| 4 | bool | `left_button_down` | 左键是否按下（false=抬起，true=按下） |
| 5 | bool | `right_button_down` | 右键是否按下（false=抬起，true=按下） |
| 6 | uint32 | `keyboard_value` | 键盘按键位掩码 |
| 7 | bool | `mid_button_down` | 中键是否按下（false=抬起，true=按下） |

`keyboard_value`：数据类型虽为 uint32，实际仅需发送 bit 0 ~ bit 15，每个 bit 对应一个按键，0 为未按下、1 为按下。

| bit | 按键 | bit | 按键 |
|---|---|---|---|
| 0 | W | 8 | R |
| 1 | S | 9 | F |
| 2 | A | 10 | G |
| 3 | D | 11 | Z |
| 4 | Shift | 12 | X |
| 5 | Ctrl | 13 | C |
| 6 | Q | 14 | V |
| 7 | E | 15 | B |

**结构体**

```proto
message KeyboardMouseControl {
  optional int32  mouse_x = 1;
  optional int32  mouse_y = 2;
  optional int32  mouse_z = 3;
  optional bool   left_button_down = 4;
  optional bool   right_button_down = 5;
  optional uint32 keyboard_value = 6;
  optional bool   mid_button_down = 7;
}
```

---

### CustomControl

| 项 | 值 |
|---|---|
| topic_name | `CustomControl` |
| 方向 | 自定义终端 → 图传链路 → 机器人 |
| 最高 QoS | 1 |
| 频率 | 75Hz |
| 用途 | 最大 60 字节的自定义数据 |

该信息将通过图传链路以 `0x0311` 命令字发送给机器人。

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | bytes | `data` | 最大 60 字节的自定义数据 |

**结构体**

```proto
message CustomControl {
  optional bytes data = 1;
}
```

---

### MapClickCmd

| 项 | 值 |
|---|---|
| topic_name | `MapClickCmd` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送；发送频率限制与「1.3.1 选手端下发数据」一致 |
| 用途 | 地图点击标记指令 |

> 1.3.1 的限速规定：云台手经 `0x0303` 下发时，两次发送间隔不得低于 **0.5 秒**；半自动控制方式的机器人，其对应操作手经 `0x0303` 下发时，两次发送间隔不得低于 **3 秒**。

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `is_send_all` | 发送范围（0=指定客户端，1=除哨兵，2=包含哨兵） |
| 2 | bytes | `robot_id` | 目标机器人 ID 列表（固定 7 字节）。如发送给己方重装和哨兵机器人：红方填 `1, 7`，蓝方填 `101, 107`，后续填 0 |
| 3 | uint32 | `mode` | 标记类型（1=攻击，2=防御，3=警戒，4=自定义） |
| 4 | uint32 | `enemy_id` | 标定的对方 ID |
| 5 | uint32 | `ascii` | 自定义图标 ASCII 码（仅在标记类型为 4：自定义时有效）。仅允许发送官方选手端可接受的字母，范围为从（含）"C" 到 "L"、"N"、"O"、"Q" 到 "Z" |
| 6 | uint32 | `type` | 标记模式（1=地图，2=对方机器人） |
| 7 | float | `map_x` | 地图坐标 X |
| 8 | float | `map_y` | 地图坐标 Y |

> **【原文注】** 字段 3 在 field 表里叫「标记类型（攻击/防御/警戒/自定义）」，但 proto 中命名为 `mode`；字段 6 在表里叫「标记模式（地图/对方机器人）」，proto 中却叫 `type`。两个名字与语义是交叉的，按 tag 号对齐即可，勿按字段名猜语义。

**结构体**

```proto
message MapClickCmd {
  optional uint32 is_send_all = 1;
  optional bytes  robot_id     = 2;
  optional uint32 mode         = 3;
  optional uint32 enemy_id     = 4;
  optional uint32 ascii        = 5;
  optional uint32 type         = 6;
  optional float  map_x        = 7;
  optional float  map_y        = 8;
}
```

---

### AssemblyCommand

| 项 | 值 |
|---|---|
| topic_name | `AssemblyCommand` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送，发送频率最高为 10Hz |
| 用途 | 重装装配指令 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `operation` | 装配操作类型（0=开始兑换，1=确认装配，2=取消装配） |
| 2 | uint32 | `difficulty` | 选中的装配难度 |

**结构体**

```proto
message AssemblyCommand {
  optional uint32 operation  = 1;
  optional uint32 difficulty = 2;
}
```

---

### RobotPerformanceSelectionCommand

| 项 | 值 |
|---|---|
| topic_name | `RobotPerformanceSelectionCommand` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送，发送频率最高为 10Hz |
| 用途 | 地面机器人选择性能体系或控制方式 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `shooter` | 发射机构性能体系（枚举值见 `RobotStaticStatus`） |
| 2 | uint32 | `chassis` | 底盘性能体系（枚举值见 `RobotStaticStatus`） |
| 3 | uint32 | `sentry_control` | 哨兵控制方式选择 |

`sentry_control` 枚举：`0` = 哨兵自动控制；`1` = 哨兵半自动控制。

**结构体**

```proto
message RobotPerformanceSelectionCommand {
  optional uint32 shooter        = 1;
  optional uint32 chassis        = 2;
  optional uint32 sentry_control = 3;
}
```

---

### CommonCommand

| 项 | 值 |
|---|---|
| topic_name | `CommonCommand` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送，发送频率最高为 10Hz |
| 用途 | 机器人多种常用指令 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `cmd_type` | 命令类型 |
| 2 | uint32 | `param` | 命令类型对应的参数 |

`cmd_type` 枚举：

| 值 | 含义 |
|---|---|
| 1 | 兑换 17mm 发弹量（仅剩余金币足以兑换时有效；该命令类型对应的参数值必须为 10 的倍数） |
| 2 | 兑换 42mm 发弹量（仅剩余金币足以兑换时有效） |
| 3 | 确认复活（若此时复活读条完成将立即复活） |
| 4 | 兑换立即复活（若符合兑换立即复活的规则要求，则立即消耗金币兑换立即复活） |
| 5 | 远程兑换允许发弹量（若符合规则要求，则立即消耗金币远程兑换允许发弹量） |
| 6 | 远程兑换血量（若符合规则要求，则立即消耗金币远程兑换血量） |

**结构体**

```proto
message CommonCommand {
  optional uint32 cmd_type = 1;
  optional uint32 param    = 2;
}
```

---

### RuneActivateCommand

| 项 | 值 |
|---|---|
| topic_name | `RuneActivateCommand` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送，发送频率最高为 10Hz |
| 用途 | 能量机关激活指令 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `activate` | 激活（1=开启） |

**结构体**

```proto
message RuneActivateCommand {
  optional uint32 activate = 1;
}
```

---

### DartCommand

| 项 | 值 |
|---|---|
| topic_name | `DartCommand` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送，发送频率最高为 10Hz |
| 用途 | 飞镖控制指令 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `target_id` | 目标 ID（1=前哨站，2=基地固定目标，3=基地随机固定目标，4=基地随机移动目标，5=基地末端移动目标） |
| 2 | bool | `open` | 闸门开关 |
| 3 | bool | `launch_confirm` | 是否确认发射（默认为 0，1 为确认发射） |

**结构体**

```proto
message DartCommand {
  optional uint32 target_id      = 1;
  optional bool   open           = 2;
  optional bool   launch_confirm = 3;
}
```

---

### SentryCtrlCommand

| 项 | 值 |
|---|---|
| topic_name | `SentryCtrlCommand` |
| 方向 | 自定义终端 → 服务器 |
| 最高 QoS | 1 |
| 频率 | 触发式发送，发送频率最高为 10Hz |
| 用途 | 哨兵控制指令请求 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `command_id` | 指令编号（0=无效），枚举见下 |

`command_id` 枚举（**原文无 `2`，属正常，不必补**）：

| 值 | 含义 | 值 | 含义 |
|---|---|---|---|
| 0 | 无效 | 7 | 切换为进攻姿态 |
| 1 | 补血点补弹 | 8 | 切换为防御姿态 |
| 3 | 远程补弹 | 9 | 切换为移动姿态 |
| 4 | 远程回血 | 10 | 切换为强化进攻姿态 |
| 5 | 确认复活 | 11 | 切换为强化防御姿态 |
| 6 | 确认花费金币复活 | 12 | 切换为强化移动姿态 |

**结构体**

```proto
message SentryCtrlCommand {
  optional uint32 command_id = 1;
}
```

---

## 3. 接收组 · 详细定义

### GameStatus

| 项 | 值 |
|---|---|
| topic_name | `GameStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 5Hz |
| 用途 | 同步比赛全局状态信息 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `current_round` | 当前局号（从 1 开始） |
| 2 | uint32 | `total_rounds` | 总局数 |
| 3 | uint32 | `red_score` | 红方得分 |
| 4 | uint32 | `blue_score` | 蓝方得分 |
| 5 | uint32 | `current_stage` | 当前阶段（枚举见下） |
| 6 | int32 | `stage_countdown_sec` | 当前阶段剩余时间（秒） |
| 7 | int32 | `stage_elapsed_sec` | 当前阶段已过时间（秒） |
| 8 | bool | `is_paused` | 是否暂停 |
| 9 | uint32 | `game_result` | 当局胜者。0 为平局，1 为红方胜利，2 为蓝方胜利。仅在「当前阶段」为「比赛结算中」时该值有意义，其余时间发送 255 |
| 10 | uint32 | `end_reason` | 比赛结束原因。仅在「当前阶段」为「比赛结算中」时该值有意义，其余时间发送 255 |

`current_stage` 枚举：`0` 未开始比赛；`1` 准备阶段；`2` 十五秒裁判系统自检阶段；`3` 五秒倒计时；`4` 比赛中；`5` 比赛结算中。

`end_reason` 枚举：`1` 基地被摧毁；`2` 控制度；`3` 全队总攻击伤害；`4` 全队机器人总剩余血量；`5` 平局；`6` 异常终止；`7` 主裁判手动判定胜负。

**结构体**

```proto
message GameStatus {
  optional uint32 current_round        = 1;
  optional uint32 total_rounds         = 2;
  optional uint32 red_score            = 3;
  optional uint32 blue_score           = 4;
  optional uint32 current_stage        = 5;
  optional int32  stage_countdown_sec  = 6;
  optional int32  stage_elapsed_sec    = 7;
  optional bool   is_paused            = 8;
  optional uint32 game_result          = 9;
  optional uint32 end_reason           = 10;
}
```

---

### GlobalUnitStatus

| 项 | 值 |
|---|---|
| topic_name | `GlobalUnitStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 同步全局胜负相关状态 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `base_health` | 己方基地当前血量（0=已摧毁） |
| 2 | uint32 | `base_status` | 己方基地状态（枚举见下） |
| 3 | uint32 | `base_shield` | 己方基地阵地控制状态（1=被己方完全控制，2=被对方临时控制） |
| 4 | uint32 | `outpost_R_health` | R 前哨站当前血量（0=已摧毁） |
| 5 | uint32 | `outpost_R_status` | R 前哨站对应的前哨阵地控制状态（1=己方完全，2=己方临时，3=对方临时，4=对方完全） |
| 6 | uint32 | `enemy_base_health` | 对方基地当前血量（0=已摧毁） |
| 7 | uint32 | `enemy_base_status` | 对方基地状态（枚举同 `base_status`） |
| 8 | uint32 | `enemy_base_shield` | 对方基地阵地控制状态（1=被对方完全控制，2=被己方临时控制） |
| 9 | uint32 | `outpost_B_health` | B 前哨站当前血量（0=已摧毁） |
| 10 | uint32 | `outpost_B_status` | B 前哨站对应的前哨阵地控制状态（1=己方完全，2=己方临时，3=对方临时，4=对方完全） |
| 11 | repeated uint32 | `robot_health` | 所有机器人血量，按「己方 1 号、3 号、4 号、7 号，对方 1 号、3 号、4 号、7 号」顺序发送 |
| 12 | repeated int32 | `robot_bullets` | 己方机器人剩余累计发弹量 |
| 13 | uint32 | `total_damage_ally` | 己方累计总伤害 |
| 14 | uint32 | `total_damage_enemy` | 对方累计总伤害 |
| 15 | uint32 | `total_CP_ally` | 己方累计控制度 |
| 16 | uint32 | `total_CP_enemy` | 对方累计控制度 |

`base_status` / `enemy_base_status` 枚举：`0` 拥有 100% 防御增益；`1` 不拥有 100% 防御增益，护甲未展开；`2` 不拥有 100% 防御增益，护甲展开。

**结构体**

```proto
message GlobalUnitStatus {
  optional uint32 base_health        = 1;
  optional uint32 base_status        = 2;
  optional uint32 base_shield        = 3;
  optional uint32 outpost_R_health   = 4;
  optional uint32 outpost_R_status   = 5;
  optional uint32 enemy_base_health  = 6;
  optional uint32 enemy_base_status  = 7;
  optional uint32 enemy_base_shield  = 8;
  optional uint32 outpost_B_health   = 9;
  optional uint32 outpost_B_status   = 10;
  repeated uint32 robot_health       = 11;
  repeated int32  robot_bullets      = 12;
  optional uint32 total_damage_ally  = 13;
  optional uint32 total_damage_enemy = 14;
  optional uint32 total_CP_ally      = 15;
  optional uint32 total_CP_enemy     = 16;
}
```

---

### GlobalLogisticsStatus

| 项 | 值 |
|---|---|
| topic_name | `GlobalLogisticsStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 同步全局后勤信息 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `remaining_economy` | 己方当前经济 |
| 2 | uint64 | `total_economy_obtained` | 己方累计总经济 |
| 3 | uint32 | `tech_level` | 己方科技等级（重装机器人已成功装配的最高难度） |
| 4 | uint32 | `encryption_level` | 己方加密等级（雷达解析信息波机制） |

**结构体**

```proto
message GlobalLogisticsStatus {
  optional uint32 remaining_economy      = 1;
  optional uint64 total_economy_obtained = 2;
  optional uint32 tech_level             = 3;
  optional uint32 encryption_level       = 4;
}
```

---

### GlobalSpecialMechanism

| 项 | 值 |
|---|---|
| topic_name | `GlobalSpecialMechanism` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 同步正在生效的全局特殊机制 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | repeated uint32 | `mechanism_id` | 正在生效的机制 ID 列表 |
| 2 | repeated int32 | `mechanism_time_sec` | 对应的时间参数（秒） |

`mechanism_id` 枚举：

| 值 | 含义 |
|---|---|
| 1 | 己方堡垒被对方激活计时 |
| 2 | 对方堡垒被己方激活计时 |
| 3 | R 前哨阵地激活计时 |
| 4 | B 前哨阵地激活计时 |
| 5 | R 基地阵地激活计时 |
| 6 | B 基地阵地激活计时 |

计时符号表示方向：`< 0` 为对方正在推进激活进度，`> 0` 为己方正在推进激活进度，`0` 为激活状态无变化（原文注：「下同」）。

**结构体**

```proto
message GlobalSpecialMechanism {
  repeated uint32 mechanism_id       = 1;
  repeated int32  mechanism_time_sec = 2;
}
```

---

### Event

| 项 | 值 |
|---|---|
| topic_name | `Event` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 触发式发送 |
| 用途 | 全局事件通知 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | int32 | `event_id` | 事件编号（枚举见下） |
| 2 | string | `param` | 事件参数（含义随 `event_id` 变化） |

`event_id` 枚举：

| 值 | 事件 | 参数含义 |
|---|---|---|
| 1 | 击杀事件 | 参数 1 为被击杀者，参数 2 为击杀者。如 `"1,101"` 代表蓝方重装击毁红方重装 |
| 2 | 前哨站被摧毁事件 | 参数值为被击毁的目标 id（红方前哨站为 11，蓝方前哨站为 111） |
| 3 | 大能量机关被成功激活 | 仅大能量机关被激活时触发。参数 1 为激活成功臂数，参数 2 为平均环数（浮点），如 `"10,9.6"` |
| 4 | 能量机关进入已激活状态 | 激活类型：1=小能量机关，2=大能量机关 |
| 5 | R 前哨阵地改变控制状态 | 1=红方临时控制，2=红方完全控制，3=蓝方临时控制，4=蓝方完全控制 |
| 6 | B 前哨阵地改变控制状态 | 同上 |
| 7 | R 基地阵地改变控制方 | 1=红方完全控制，2=蓝方临时控制 |
| 8 | B 基地阵地改变控制方 | 1=蓝方完全控制，2=红方临时控制 |
| 9 | 飞镖命中 | 参数 1：1=红方命中，2=蓝方命中；参数 2：1=击中前哨站，2=击中基地固定目标，3=击中基地随机固定目标，4=击中基地随机移动目标，5=击中基地末端移动目标 |
| 10 | 对方飞镖闸门开启 | 无参数 |
| 11 | 基地遭到攻击 | 无参数值，每次触发存在 5s 内置冷却 |
| 12 | 对方前哨站停转 | 无参数 |
| 13 | 对方基地护甲展开 | 无参数 |
| 14 | 重装战亡临时激活 | 进入强制退出缓冲期 |
| 15 | 装配结果事件 | 0 成功，1 拔出，2 超时，3 离开装配区过久，4 重装战亡，5 四级难度未满足完成协作时限，6 主动退出，7 完成装配但结算时未检测到有能量单元，8 缓冲期到期（装配流程强制结束） |
| 16 | 重装机器人成功安装模块化 42mm 发射机构 | — |

**结构体**

```proto
message Event {
  optional int32  event_id = 1;
  optional string param    = 2;
}
```

---

### RobotInjuryStat

| 项 | 值 |
|---|---|
| topic_name | `RobotInjuryStat` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 机器人一次存活期间累计受伤统计 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `total_damage` | 该次存活累计受伤总计 |
| 2 | uint32 | `collision_damage` | 撞击伤害 |
| 3 | uint32 | `small_projectile_damage` | 17mm 弹丸伤害 |
| 4 | uint32 | `large_projectile_damage` | 42mm 弹丸伤害 |
| 5 | uint32 | `dart_splash_damage` | 飞镖溅射伤害 |
| 6 | uint32 | `module_offline_damage` | 模块离线扣血 |
| 7 | uint32 | `offline_damage` | 异常离线扣血 |
| 8 | uint32 | `penalty_damage` | 判罚扣血 |
| 9 | uint32 | `server_kill_damage` | 服务器强制使其战亡扣血 |
| 10 | uint32 | `killer_id` | 击杀者 ID（若未检测到击杀者，值为 0） |

**结构体**

```proto
message RobotInjuryStat {
  optional uint32 total_damage            = 1;
  optional uint32 collision_damage        = 2;
  optional uint32 small_projectile_damage = 3;
  optional uint32 large_projectile_damage = 4;
  optional uint32 dart_splash_damage      = 5;
  optional uint32 module_offline_damage   = 6;
  optional uint32 offline_damage          = 7;
  optional uint32 penalty_damage          = 8;
  optional uint32 server_kill_damage      = 9;
  optional uint32 killer_id               = 10;
}
```

---

### RobotRespawnStatus

| 项 | 值 |
|---|---|
| topic_name | `RobotRespawnStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 机器人复活状态同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | bool | `is_pending_respawn` | 是否处于待复活状态 |
| 2 | uint32 | `total_respawn_progress` | 复活所需总读条 |
| 3 | uint32 | `current_respawn_progress` | 当前复活读条进度 |
| 4 | bool | `can_free_respawn` | 是否可以免费复活 |
| 5 | uint32 | `gold_cost_for_respawn` | 花费金币复活所需金币数 |
| 6 | bool | `can_pay_for_respawn` | 是否允许花费金币复活 |

**结构体**

```proto
message RobotRespawnStatus {
  optional bool   is_pending_respawn       = 1;
  optional uint32 total_respawn_progress   = 2;
  optional uint32 current_respawn_progress = 3;
  optional bool   can_free_respawn         = 4;
  optional uint32 gold_cost_for_respawn    = 5;
  optional bool   can_pay_for_respawn      = 6;
}
```

---

### RobotStaticStatus

| 项 | 值 |
|---|---|
| topic_name | `RobotStaticStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 机器人固定属性和配置 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `connection_state` | 连接状态（0=未连接，1=已连接） |
| 2 | uint32 | `field_state` | 上场状态（0=已上场，1=未上场） |
| 3 | uint32 | `alive_state` | 存活状态（0=未知，1=存活，2=战亡） |
| 4 | uint32 | `robot_id` | 机器人编号（ID 见附录 B） |
| 5 | uint32 | `robot_type` | 机器人类型 |
| 6 | uint32 | `performance_system_shooter` | 性能体系-发射机构 |
| 7 | uint32 | `performance_system_chassis` | 性能体系-底盘 |
| 8 | uint32 | `level` | 当前等级 |
| 9 | uint32 | `max_health` | 最大血量 |
| 10 | uint32 | `max_heat` | 最大热量 |
| 11 | float | `heat_cooldown_rate` | 热量冷却速率（每秒） |
| 12 | uint32 | `max_power` | 最大功率 |
| 13 | uint32 | `max_buffer_energy` | 最大缓冲能量 |
| 14 | uint32 | `max_chassis_energy` | 最大底盘能量 |

`performance_system_shooter` 枚举：`1` 冷却优先；`2` 爆发优先。
`performance_system_chassis` 枚举：`1` 血量优先；`2` 功率优先。

**结构体**

```proto
message RobotStaticStatus {
  optional uint32 connection_state          = 1;
  optional uint32 field_state               = 2;
  optional uint32 alive_state               = 3;
  optional uint32 robot_id                  = 4;
  optional uint32 robot_type                = 5;
  optional uint32 performance_system_shooter = 6;
  optional uint32 performance_system_chassis = 7;
  optional uint32 level                     = 8;
  optional uint32 max_health                = 9;
  optional uint32 max_heat                  = 10;
  optional float  heat_cooldown_rate        = 11;
  optional uint32 max_power                 = 12;
  optional uint32 max_buffer_energy         = 13;
  optional uint32 max_chassis_energy        = 14;
}
```

---

### RobotDynamicStatus

| 项 | 值 |
|---|---|
| topic_name | `RobotDynamicStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 10Hz |
| 用途 | 机器人实时数据 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `current_health` | 当前血量 |
| 2 | float | `current_heat` | 当前热量 |
| 3 | float | `last_projectile_fire_rate` | 上一次弹丸射速 |
| 4 | uint32 | `reserved` | 保留位 |
| 5 | uint32 | `current_buffer_energy` | 当前缓冲能量 |
| 6 | uint32 | `current_experience` | 当前经验值 |
| 7 | uint32 | `experience_for_upgrade` | 距离下一次升级仍需获得的经验 |
| 8 | uint32 | `total_projectiles_fired` | 累计已发弹量 |
| 9 | uint32 | `remaining_ammo` | 剩余允许发弹量 |
| 10 | bool | `is_out_of_combat` | 是否处于脱战状态 |
| 11 | uint32 | `out_of_combat_countdown` | 脱战状态倒计时 |
| 12 | bool | `can_remote_heal` | 是否可以远程补血 |
| 13 | bool | `can_remote_ammo` | 是否可以远程补弹 |

**结构体**

```proto
message RobotDynamicStatus {
  optional uint32 current_health          = 1;
  optional float  current_heat            = 2;
  optional float  last_projectile_fire_rate = 3;
  optional uint32 reserved                = 4;
  optional uint32 current_buffer_energy   = 5;
  optional uint32 current_experience      = 6;
  optional uint32 experience_for_upgrade  = 7;
  optional uint32 total_projectiles_fired = 8;
  optional uint32 remaining_ammo          = 9;
  optional bool   is_out_of_combat        = 10;
  optional uint32 out_of_combat_countdown = 11;
  optional bool   can_remote_heal         = 12;
  optional bool   can_remote_ammo         = 13;
}
```

---

### RobotModuleStatus

| 项 | 值 |
|---|---|
| topic_name | `RobotModuleStatus` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 机器人各模块运行状态 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `power_manager` | 电源管理模块状态 |
| 2 | uint32 | `rfid` | RFID 模块状态 |
| 3 | uint32 | `light_strip` | 灯条模块状态 |
| 4 | uint32 | `small_shooter` | 17mm 测速模块状态 |
| 5 | uint32 | `big_shooter` | 42mm 测速模块状态 |
| 6 | uint32 | `uwb` | 定位模块状态 |
| 7 | uint32 | `armor` | 装甲模块状态 |
| 8 | uint32 | `video_transmission` | 图传模块状态 |
| 9 | uint32 | `capacitor` | 电容模块状态 |
| 10 | uint32 | `main_controller` | 主控状态 |
| 11 | uint32 | `modular_42mm` | 是否拥有模块化 42mm 发射机构安装权限（仅重装）（0=未拥有，1=拥有） |

字段 1~10 的状态枚举：`0` 离线；`1` 在线；`2` 因安装不规范被视为离线。

**结构体**

```proto
message RobotModuleStatus {
  optional uint32 power_manager      = 1;
  optional uint32 rfid               = 2;
  optional uint32 light_strip        = 3;
  optional uint32 small_shooter      = 4;
  optional uint32 big_shooter        = 5;
  optional uint32 uwb                = 6;
  optional uint32 armor              = 7;
  optional uint32 video_transmission = 8;
  optional uint32 capacitor          = 9;
  optional uint32 main_controller    = 10;
  optional uint32 modular_42mm       = 11;
}
```

---

### RobotPosition

| 项 | 值 |
|---|---|
| topic_name | `RobotPosition` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 机器人空间坐标和朝向 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | float | `x` | 世界坐标 X 轴 |
| 2 | float | `y` | 世界坐标 Y 轴 |
| 3 | float | `z` | 世界坐标 Z 轴 |
| 4 | float | `yaw` | 本机器人测速模块的朝向，单位：度，正北为 0 度 |
| 5 | uint32 | `robot_id` | 接收到的坐标归属机器人；`7` / `107` 为哨兵机器人（仅指挥手客户端生效） |

**结构体**

```proto
message RobotPosition {
  optional float  x        = 1;
  optional float  y        = 2;
  optional float  z        = 3;
  optional float  yaw      = 4;
  optional uint32 robot_id = 5;
}
```

---

### Buff

| 项 | 值 |
|---|---|
| topic_name | `Buff` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 获得增益时触发发送，此后 1Hz 定频发送直到失去增益 |
| 用途 | Buff 效果信息 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `robot_id` | 机器人 ID |
| 2 | uint32 | `buff_type` | Buff 类型（枚举见下） |
| 3 | int32 | `buff_level` | Buff 增益值。buff 类型为 1、2、4、5 时此值为**百分比**（如防御增益 50 表示 50% 防御增益）；类型为 3 时此值为**直接值**，值为 x 表示热量冷却增加 x/s；增益值若非整数按四舍五入原则取整 |
| 4 | uint32 | `buff_max_time` | Buff 最大剩余时间 |
| 5 | uint32 | `buff_left_time` | Buff 剩余时间 |

`buff_type` 枚举：

| 值 | 含义 |
|---|---|
| 1 | 攻击增益 |
| 2 | 防御增益 / 负防御增益（易伤） |
| 3 | 射击热量冷却增益 |
| 4 | 底盘功率增益 |
| 5 | 回血增益 |
| 6 | 可兑换允许发弹量 |
| 7 | 地形跨越增益（预备）——指检测到地形跨越增益点（高地）前半部分的场地交互模块，但未获取实际地形跨越增益（高地）的情况 |

**结构体**

```proto
message Buff {
  optional uint32 robot_id       = 1;
  optional uint32 buff_type      = 2;
  optional int32  buff_level     = 3;
  optional uint32 buff_max_time  = 4;
  optional uint32 buff_left_time = 5;
}
```

---

### PenaltyInfo

| 项 | 值 |
|---|---|
| topic_name | `PenaltyInfo` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 触发式发送 |
| 用途 | 判罚信息同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `penalty_type` | 当前受罚类型（枚举见下） |
| 2 | uint32 | `penalty_effect_sec` | 当前受罚效果时长（秒） |
| 3 | uint32 | `total_penalty_num` | 当前判罚数量 |

`penalty_type` 枚举：`1` 黄牌；`2` 双方黄牌；`3` 红牌；`4` 超功率；`5` 超热量；`6` 超射速。

**结构体**

```proto
message PenaltyInfo {
  optional uint32 penalty_type       = 1;
  optional uint32 penalty_effect_sec = 2;
  optional uint32 total_penalty_num  = 3;
}
```

---

### RobotPathPlanInfo

| 项 | 值 |
|---|---|
| topic_name | `RobotPathPlanInfo` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 触发式发送 |
| 用途 | 哨兵轨迹规划信息 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `intention` | 哨兵意图（1=攻击，2=防守，3=移动） |
| 2 | uint32 | `start_pos_x` | 起始点 X 坐标（分米） |
| 3 | uint32 | `start_pos_y` | 起始点 Y 坐标（分米） |
| 4 | repeated int32 | `offset_x` | 相对起始点 X 增量数组（-128 ~ +127，长度 49），`packed = true` |
| 5 | repeated int32 | `offset_y` | 相对起始点 Y 增量数组（-128 ~ +127，长度 49），`packed = true` |
| 6 | uint32 | `sender_id` | 发送者 ID |

**结构体**

```proto
message RobotPathPlanInfo {
  optional uint32 intention   = 1;
  optional uint32 start_pos_x = 2;
  optional uint32 start_pos_y = 3;
  repeated int32  offset_x    = 4 [packed = true];
  repeated int32  offset_y    = 5 [packed = true];
  optional uint32 sender_id   = 6;
}
```

---

### MapClickInfo

| 项 | 值 |
|---|---|
| topic_name | `MapClickInfo` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 触发式发送；发送频率限制与「1.3.1 选手端下发数据」一致 |
| 用途 | 地图点击标记信息同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `is_send_all` | 发送范围（0=指定客户端，1=除哨兵，2=包含哨兵） |
| 2 | bytes | `robot_id` | 目标机器人 ID 列表（固定 7 字节）。如发送给己方重装和哨兵机器人：红方填 `1, 7`，蓝方填 `101, 107`，后续填 0 |
| 3 | uint32 | `mode` | 标记类型（1=攻击，2=防御，3=警戒，4=自定义） |
| 4 | uint32 | `enemy_id` | 标定的对方 ID |
| 5 | uint32 | `ascii` | 自定义图标 ASCII 码（仅在标记类型为 4：自定义时有效）。仅允许发送官方选手端可接受的字母，范围为从（含）"C" 到 "L"、"N"、"O"、"Q" 到 "Z" |
| 6 | uint32 | `type` | 标记模式（1=地图，2=对方机器人） |
| 7 | float | `map_x` | 地图坐标 X |
| 8 | float | `map_y` | 地图坐标 Y |

> **【原文注】** 与 `MapClickCmd` 相同：字段 3（标记类型）在 proto 中名为 `mode`，字段 6（标记模式）名为 `type`，字段名与语义交叉。

**结构体**

```proto
message MapClickInfo {
  optional uint32 is_send_all = 1;
  optional bytes  robot_id     = 2;
  optional uint32 mode         = 3;
  optional uint32 enemy_id     = 4;
  optional uint32 ascii        = 5;
  optional uint32 type         = 6;
  optional float  map_x        = 7;
  optional float  map_y        = 8;
}
```

---

### RadarInfoToClient

| 项 | 值 |
|---|---|
| topic_name | `RadarInfoToClient` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 触发式发送 |
| 用途 | 雷达发送的机器人位置信息 |

**外层字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | repeated `RadarSingleRobotInfo` | `RadarSingleRobotInfo` | 雷达发送的所有机器人位置信息及特殊标识情况。每次触发发送时，均按「对方 1 号、3 号、4 号、7 号；己方 1 号、3 号、4 号、7 号」的排列顺序，发送 8 组下述结构的数据包 |

**内层结构 `RadarSingleRobotInfo` 字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `target_pos_x` | 机器人位置 x（cm） |
| 2 | uint32 | `target_pos_y` | 机器人位置 y（cm） |
| 3 | uint32 | `is_high_light` | 机器人是否特殊标识（0=否；1=是；2=是，但目标机器人此时定位模块为离线状态） |

> **【原文注】** 原文结构体写作 `repeated uint32 RadarSingleRobotInfo;`（类型为 uint32，且**未写 tag 编号**），但下文又定义了对应的 message，显然应是 `repeated RadarSingleRobotInfo` 且 tag = 1。按语义实现时需与官方确认。

**结构体（原文照录）**

```proto
message RadarInfoToClient {
  repeated uint32 RadarSingleRobotInfo;
}

message RadarSingleRobotInfo
{
  optional uint32 target_pos_x = 1;
  optional uint32 target_pos_y = 2;
  optional uint32 is_high_light = 3;
}
```

---

### CustomByteBlock

| 项 | 值 |
|---|---|
| topic_name | `CustomByteBlock` |
| 方向 | 机器人 → 图传链路 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 50Hz |
| 用途 | 自定义数据流（对应图传链路 `0x0310`，载荷上限 300 字节） |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | bytes | `data` | 最大为 2.4 kbit（= 300 字节）的自定义数据包 |

**结构体**

```proto
message CustomByteBlock {
  optional bytes data = 1;
}
```

---

### TechCoreMotionStateSync

| 项 | 值 |
|---|---|
| topic_name | `TechCoreMotionStateSync` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 科技核心运动状态同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `maximum_difficulty_level` | 当前可选择的最高装配难度等级 |
| 2 | uint32 | `basic_state` | 科技核心基础状态（1=初始位置，2=运动中，3=已到达对应位姿） |
| 3 | uint32 | `putin_state` | 能量单元放入状态（0=未放入，1=已放入） |
| 4 | uint32 | `move_state` | 能量单元平移状态（0=未平移，1=已平移） |
| 5 | uint32 | `rotate_state` | 能量单元旋转状态（0=未旋转，1=已旋转） |
| 6 | uint32 | `enemy_core_status` | 对方科技核心状态（仅己方四级装配可用时有效；0=对方没有正在装配，1=对方正在装配非四级难度，2=对方正在装配四级难度） |
| 7 | uint32 | `remain_time_all` | 己方装配总剩余时长（仅己方正在四级装配时有效） |
| 8 | uint32 | `remain_time_step` | 己方单个步骤装配间隔剩余时长（己方正在四级装配时，或战亡临时激活缓冲期有效） |

**结构体**

```proto
message TechCoreMotionStateSync {
  optional uint32 maximum_difficulty_level = 1;
  optional uint32 basic_state              = 2;
  optional uint32 putin_state              = 3;
  optional uint32 move_state               = 4;
  optional uint32 rotate_state             = 5;
  optional uint32 enemy_core_status        = 6;
  optional uint32 remain_time_all          = 7;
  optional uint32 remain_time_step         = 8;
}
```

---

### RobotPerformanceSelectionSync

| 项 | 值 |
|---|---|
| topic_name | `RobotPerformanceSelectionSync` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 性能体系状态同步（**仅适用于步兵**；2.1 概览里的「步兵/重装」是笔误，正文为步兵） |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `shooter` | 发射机构性能体系 |
| 2 | uint32 | `chassis` | 底盘性能体系 |
| 3 | uint32 | `sentry_control` | 哨兵控制方式选择（0=哨兵自动控制，1=哨兵半自动控制） |

**结构体**

```proto
message RobotPerformanceSelectionSync {
  optional uint32 shooter        = 1;
  optional uint32 chassis        = 2;
  optional uint32 sentry_control = 3;
}
```

---

### RuneStatusSync

| 项 | 值 |
|---|---|
| topic_name | `RuneStatusSync` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 能量机关状态同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `rune_status` | 当前能量机关状态枚举（1=未激活，2=正在激活，3=已激活） |
| 2 | uint32 | `activated_arms` | 当前已激活的灯臂数量 |
| 3 | float | `average_rings` | 平均环数（仅大能量机关正在激活状态有效，其余状态该值为 0） |

**结构体**

```proto
message RuneStatusSync {
  optional uint32 rune_status    = 1;
  optional uint32 activated_arms = 2;
  optional float  average_rings  = 3;
}
```

---

### SentryStatusSync

| 项 | 值 |
|---|---|
| topic_name | `SentryStatusSync` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 哨兵姿态相关信息同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `posture_id` | 姿态 ID（1=进攻姿态，2=防御姿态，3=移动姿态） |
| 2 | bool | `is_weakened` | 是否弱化 |
| 3 | bool | `is_powered` | 是否强化 |

**结构体**

```proto
message SentryStatusSync {
  optional uint32 posture_id  = 1;
  optional bool   is_weakened = 2;
  optional bool   is_powered  = 3;
}
```

---

### DartSelectTargetStatusSync

| 项 | 值 |
|---|---|
| topic_name | `DartSelectTargetStatusSync` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 1Hz |
| 用途 | 飞镖目标选择状态同步 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `target_id` | 目标 ID |
| 2 | uint32 | `open` | 闸门状态（0=关闭，1=开启中，2=已开启） |

**结构体**

```proto
message DartSelectTargetStatusSync {
  optional uint32 target_id = 1;
  optional uint32 open      = 2;
}
```

---

### SentryCtrlResult

| 项 | 值 |
|---|---|
| topic_name | `SentryCtrlResult` |
| 方向 | 服务器 → 自定义终端 |
| 最高 QoS | 1 |
| 频率 | 触发式发送 |
| 用途 | 哨兵控制指令结果反馈 |

**字段**

| 编号 | 类型 | 字段名 | 说明 |
|---|---|---|---|
| 1 | uint32 | `command_id` | 对应的指令编号（枚举同 `SentryCtrlCommand.command_id`） |
| 2 | uint32 | `result_code` | 执行结果码（0=成功，其他=失败） |

**结构体**

```proto
message SentryCtrlResult {
  optional uint32 command_id  = 1;
  optional uint32 result_code = 2;
}
```

---

## 附录 A：合并 `.proto`（可直接用于 `protoc`）

> 字段名与 tag 编号严格照录原文。原文所有字段均为 proto3 `optional`。
> `RadarInfoToClient` 的外层字段在原文中写作 `repeated uint32 RadarSingleRobotInfo;`（无 tag、类型为 uint32），此处按语义修正为 `repeated RadarSingleRobotInfo RadarSingleRobotInfo = 1;`，并保留注释说明。

```proto
syntax = "proto3";

// ===== 发送组：自定义终端 → 服务器 / 图传链路 =====

message KeyboardMouseControl {
  optional int32  mouse_x = 1;
  optional int32  mouse_y = 2;
  optional int32  mouse_z = 3;
  optional bool   left_button_down = 4;
  optional bool   right_button_down = 5;
  optional uint32 keyboard_value = 6;
  optional bool   mid_button_down = 7;
}

message CustomControl {
  optional bytes data = 1;
}

message MapClickCmd {
  optional uint32 is_send_all = 1;
  optional bytes  robot_id     = 2;
  optional uint32 mode         = 3;  // 标记类型：1 攻击 2 防御 3 警戒 4 自定义
  optional uint32 enemy_id     = 4;
  optional uint32 ascii        = 5;
  optional uint32 type         = 6;  // 标记模式：1 地图 2 对方机器人
  optional float  map_x        = 7;
  optional float  map_y        = 8;
}

message AssemblyCommand {
  optional uint32 operation  = 1;
  optional uint32 difficulty = 2;
}

message RobotPerformanceSelectionCommand {
  optional uint32 shooter        = 1;
  optional uint32 chassis        = 2;
  optional uint32 sentry_control = 3;
}

message CommonCommand {
  optional uint32 cmd_type = 1;
  optional uint32 param    = 2;
}

message RuneActivateCommand {
  optional uint32 activate = 1;
}

message DartCommand {
  optional uint32 target_id      = 1;
  optional bool   open           = 2;
  optional bool   launch_confirm = 3;
}

message SentryCtrlCommand {
  optional uint32 command_id = 1;
}

// ===== 接收组：服务器 / 图传链路 → 自定义终端 =====

message GameStatus {
  optional uint32 current_round       = 1;
  optional uint32 total_rounds        = 2;
  optional uint32 red_score           = 3;
  optional uint32 blue_score          = 4;
  optional uint32 current_stage       = 5;
  optional int32  stage_countdown_sec = 6;
  optional int32  stage_elapsed_sec   = 7;
  optional bool   is_paused           = 8;
  optional uint32 game_result         = 9;
  optional uint32 end_reason          = 10;
}

message GlobalUnitStatus {
  optional uint32 base_health        = 1;
  optional uint32 base_status        = 2;
  optional uint32 base_shield        = 3;
  optional uint32 outpost_R_health   = 4;
  optional uint32 outpost_R_status   = 5;
  optional uint32 enemy_base_health  = 6;
  optional uint32 enemy_base_status  = 7;
  optional uint32 enemy_base_shield  = 8;
  optional uint32 outpost_B_health   = 9;
  optional uint32 outpost_B_status   = 10;
  repeated uint32 robot_health       = 11;
  repeated int32  robot_bullets      = 12;
  optional uint32 total_damage_ally  = 13;
  optional uint32 total_damage_enemy = 14;
  optional uint32 total_CP_ally      = 15;
  optional uint32 total_CP_enemy     = 16;
}

message GlobalLogisticsStatus {
  optional uint32 remaining_economy      = 1;
  optional uint64 total_economy_obtained = 2;
  optional uint32 tech_level             = 3;
  optional uint32 encryption_level       = 4;
}

message GlobalSpecialMechanism {
  repeated uint32 mechanism_id       = 1;
  repeated int32  mechanism_time_sec = 2;
}

message Event {
  optional int32  event_id = 1;
  optional string param    = 2;
}

message RobotInjuryStat {
  optional uint32 total_damage            = 1;
  optional uint32 collision_damage        = 2;
  optional uint32 small_projectile_damage = 3;
  optional uint32 large_projectile_damage = 4;
  optional uint32 dart_splash_damage      = 5;
  optional uint32 module_offline_damage   = 6;
  optional uint32 offline_damage          = 7;
  optional uint32 penalty_damage          = 8;
  optional uint32 server_kill_damage      = 9;
  optional uint32 killer_id               = 10;
}

message RobotRespawnStatus {
  optional bool   is_pending_respawn       = 1;
  optional uint32 total_respawn_progress   = 2;
  optional uint32 current_respawn_progress = 3;
  optional bool   can_free_respawn         = 4;
  optional uint32 gold_cost_for_respawn    = 5;
  optional bool   can_pay_for_respawn      = 6;
}

message RobotStaticStatus {
  optional uint32 connection_state           = 1;
  optional uint32 field_state                = 2;
  optional uint32 alive_state                = 3;
  optional uint32 robot_id                   = 4;
  optional uint32 robot_type                 = 5;
  optional uint32 performance_system_shooter = 6;
  optional uint32 performance_system_chassis = 7;
  optional uint32 level                      = 8;
  optional uint32 max_health                 = 9;
  optional uint32 max_heat                   = 10;
  optional float  heat_cooldown_rate         = 11;
  optional uint32 max_power                  = 12;
  optional uint32 max_buffer_energy          = 13;
  optional uint32 max_chassis_energy         = 14;
}

message RobotDynamicStatus {
  optional uint32 current_health            = 1;
  optional float  current_heat              = 2;
  optional float  last_projectile_fire_rate = 3;
  optional uint32 reserved                  = 4;
  optional uint32 current_buffer_energy     = 5;
  optional uint32 current_experience        = 6;
  optional uint32 experience_for_upgrade    = 7;
  optional uint32 total_projectiles_fired   = 8;
  optional uint32 remaining_ammo            = 9;
  optional bool   is_out_of_combat          = 10;
  optional uint32 out_of_combat_countdown   = 11;
  optional bool   can_remote_heal           = 12;
  optional bool   can_remote_ammo           = 13;
}

message RobotModuleStatus {
  optional uint32 power_manager      = 1;
  optional uint32 rfid               = 2;
  optional uint32 light_strip        = 3;
  optional uint32 small_shooter      = 4;
  optional uint32 big_shooter        = 5;
  optional uint32 uwb                = 6;
  optional uint32 armor              = 7;
  optional uint32 video_transmission = 8;
  optional uint32 capacitor          = 9;
  optional uint32 main_controller    = 10;
  optional uint32 modular_42mm       = 11;
}

message RobotPosition {
  optional float  x        = 1;
  optional float  y        = 2;
  optional float  z        = 3;
  optional float  yaw      = 4;
  optional uint32 robot_id = 5;
}

message Buff {
  optional uint32 robot_id       = 1;
  optional uint32 buff_type      = 2;
  optional int32  buff_level     = 3;
  optional uint32 buff_max_time  = 4;
  optional uint32 buff_left_time = 5;
}

message PenaltyInfo {
  optional uint32 penalty_type       = 1;
  optional uint32 penalty_effect_sec = 2;
  optional uint32 total_penalty_num  = 3;
}

message RobotPathPlanInfo {
  optional uint32 intention   = 1;
  optional uint32 start_pos_x = 2;
  optional uint32 start_pos_y = 3;
  repeated int32  offset_x    = 4 [packed = true];
  repeated int32  offset_y    = 5 [packed = true];
  optional uint32 sender_id   = 6;
}

message MapClickInfo {
  optional uint32 is_send_all = 1;
  optional bytes  robot_id     = 2;
  optional uint32 mode         = 3;  // 标记类型：1 攻击 2 防御 3 警戒 4 自定义
  optional uint32 enemy_id     = 4;
  optional uint32 ascii        = 5;
  optional uint32 type         = 6;  // 标记模式：1 地图 2 对方机器人
  optional float  map_x        = 7;
  optional float  map_y        = 8;
}

message RadarSingleRobotInfo {
  optional uint32 target_pos_x   = 1;
  optional uint32 target_pos_y   = 2;
  optional uint32 is_high_light  = 3;
}

message RadarInfoToClient {
  // 原文：repeated uint32 RadarSingleRobotInfo;（无 tag，类型疑为笔误）
  repeated RadarSingleRobotInfo RadarSingleRobotInfo = 1;
}

message CustomByteBlock {
  optional bytes data = 1;
}

message TechCoreMotionStateSync {
  optional uint32 maximum_difficulty_level = 1;
  optional uint32 basic_state              = 2;
  optional uint32 putin_state              = 3;
  optional uint32 move_state               = 4;
  optional uint32 rotate_state             = 5;
  optional uint32 enemy_core_status        = 6;
  optional uint32 remain_time_all          = 7;
  optional uint32 remain_time_step         = 8;
}

message RobotPerformanceSelectionSync {
  optional uint32 shooter        = 1;
  optional uint32 chassis        = 2;
  optional uint32 sentry_control = 3;
}

message RuneStatusSync {
  optional uint32 rune_status    = 1;
  optional uint32 activated_arms = 2;
  optional float  average_rings  = 3;
}

message SentryStatusSync {
  optional uint32 posture_id  = 1;
  optional bool   is_weakened = 2;
  optional bool   is_powered  = 3;
}

message DartSelectTargetStatusSync {
  optional uint32 target_id = 1;
  optional uint32 open      = 2;
}

message SentryCtrlResult {
  optional uint32 command_id  = 1;
  optional uint32 result_code = 2;
}
```

---

## 附录 B：ID 编号说明（协议附录二摘录）

机器人 ID：

| ID | 对象 | ID | 对象 |
|---|---|---|---|
| 1 | 红方重装机器人 | 101 | 蓝方重装机器人 |
| 2 | 保留位 | 102 | 保留位 |
| 3 / 4 / 5 | 红方步兵机器人 | 103 / 104 / 105 | 蓝方步兵机器人 |
| 6 | 红方空中系统 | 106 | 蓝方空中系统 |
| 7 | 红方哨兵机器人 | 107 | 蓝方哨兵机器人 |
| 8 | 红方飞镖 | 108 | 蓝方飞镖 |
| 9 | 红方雷达 | 109 | 蓝方雷达 |
| 10 | 红方基地 | 110 | 蓝方基地 |
| 11 | R 前哨 | 111 | B 前哨 |

选手端 ID：`0x0101` 红方重装、`0x0103`/`0x0104`/`0x0105` 红方步兵、`0x0106` 红方空中系统、`0x0165` 蓝方重装、`0x0167`/`0x0168`/`0x0169` 蓝方步兵、`0x016A` 蓝方空中系统、`0x8080` 裁判系统服务器（用于哨兵和雷达自主决策指令）。

> R 前哨为开局时红方一侧的前哨站，B 前哨为开局时蓝方一侧的前哨站；R 前哨阵地为开局时红方一侧的前哨阵地，B 前哨阵地为开局时蓝方一侧的前哨阵地（其余同理）。

---

## 附录 C：实现要点清单

1. **topic 名 = 消息名**，大小写敏感，直接用本节标题的驼峰串，无需加前缀。
2. **payload 是裸 protobuf 二进制**，不做 UTF-8 字符串转换；MqttNet 用 `PublishBinaryAsync`、`ApplicationMessage.Payload` 直接喂给 `ParseFrom`。
3. **clientID 用机器人 ID 编号**（1/3/4/7/101/103/…），不是选手端 ID。
4. **QoS 压到 1**：概览表规定上限为 1，示例代码的 `ExactlyOnce` 默认值不应照抄。
5. **频率上限**：`KeyboardMouseControl`/`CustomControl` 75Hz、`CustomByteBlock` 50Hz、`RobotDynamicStatus` 10Hz、`GameStatus` 5Hz；所有**上行**指令（`*Command`）为触发式且 ≤10Hz；`MapClick*` 受 1.3.1 的 0.5s / 3s 间隔约束。
6. **`RobotPathPlanInfo.offset_x/offset_y` 是 packed int32**，固定长度 49，值域 -128~+127。
7. **`MapClickInfo`/`MapClickCmd` 中 `mode` 与 `type` 的命名与语义交叉**，按 tag 号对齐，不要按字段名推断。本仓库实现时把 `mode` 改名 `mark_type`、`type` 改名 `mark_mode`（tag 不变）。
8. **`RadarInfoToClient` 外层字段原文有误**（`repeated uint32` 且缺 tag），实现按修正版 `repeated RadarSingleRobotInfo RadarSingleRobotInfo = 1;`；**联调时要验证**，若服务器真按 packed uint32 发，需要加容错分支。内容为 8 组 `RadarSingleRobotInfo`，顺序为「对方 1/3/4/7，己方 1/3/4/7」，与 `GlobalUnitStatus.robot_health` 的「己方在前」顺序**不同**。
9. **`GlobalSpecialMechanism` 的时间参数带符号**，符号表示推进方向。
10. **图传码流走 UDP 3334，不复用 MQTT**；8 字节包头为大端，与 protobuf 无关。
11. **只有这 32 个 topic**：名字一字不差、大小写敏感；客户端**不得自造 topic**（例如上报"我在线"），服务器不认。
12. **无认证**：没有用户名/密码/TLS，clientID 正确即可连接；一台客户端对应一台机器人，同 ID 开两条连接会被互相踢下线。
13. **协议会随赛季更新**：本整理稿对应 V1.0.0（20261029）。实现里保留协议版本号，便于对照与升级。
