# MotionPlayer / EmotePlayer 2016 闭源行为研究

> **定位：原版行为的静态逆向证据，不是当前实现状态，也不是兼容性证明。** 当前能力仍以 [MOTIONPLAYER_RESEARCH.md](MOTIONPLAYER_RESEARCH.md) 为唯一权威入口；逐项接口差距见 [MOTIONPLAYER_API_CONTRACT_MATRIX.md](MOTIONPLAYER_API_CONTRACT_MATRIX.md)。
>
> 本文只描述本地 `origin/` 中具名二进制的可复核事实。地址均为未重定位 PE 虚拟地址；不同版本不能直接套用。
>
> 最后核对：2026-08-14

## 1. 结论摘要

这轮静态分析把五个高优先级缺口从“名称/类型线索”推进到了可复核的 clean-room 数学规格：

1. 2016 `emoteplayer.dll` 不只是 TJS wrapper；它静态包含 `EPBustControl`、`EPPendControl`、`EPEyeControl`、`EPWindControl` 等 E-mote 核心控制实现。
2. `initPhysics(data.metadata)` 会解析 bust、hair、parts、eye 等控制表并创建持久控制对象，不是占位入口。
3. `eyeControl` 含真实自动眨眼状态机。已恢复字段、初始状态和 idle → close → hold → open → idle 主循环；其它 1–9 状态还承担手动/图控制。
4. `setOuterForce(name,x,y,time,easing)` 将 `bust`、`hair`、`parts` 分流到三个持久插值器；2016 原版明确接受完整字符串 `hair`。
5. `startWind(start,goal,speed,powMin,powMax)` 创建 128 槽随机阵风发生器；真正供摆动控制读取的空间查询是 `0x100cb4b0`。旧结论把 `0x100cb370` 的调试几何生成误认成力场核，现已撤回。
6. `hairScale`、`bustScale`、`partsScale` 不是装饰属性：每帧分别传入摆动/胸部控制消费者。
7. `EPBustControl` 与 `EPPendControl` 的积分、约束、输出饱和和 bend 振荡公式已逐指令恢复；同时确认两个被原版构造函数漏初始化、却在正常输出中读取的字段。
8. 控制器结果写入具名数值表；该表在下一次 `frameProgress` 的核心更新前才转入核心变量。对这个 DLL 而言，一帧 staging 是静态调用顺序的确认事实，不再只是猜测。

因此，实现这些功能**需要逆向，但不再需要盲猜接口、主状态机或 P0/P1 helper 数学**。剩余难点主要是两个原版未初始化字段的兼容策略、metadata edge/node 图结构、mode/mirror 业务语义、随机性复现，以及当前 renderer 中的最终挂接。

## 2. 样本与证据边界

| 文件 | SHA-256 | 类型 | 本文用途 |
|---|---|---|---|
| `origin/emoteplayer.dll` | `c29474887d7ef2533f3ff19e24f63a607a2e246373b9d0cf394f5d57a7a633c5` | PE32 x86，2016-02-05 | 主要对象；包含 NCB wrapper 和静态链接控制核心 |
| `origin/motionplayer.dll` | `3b5509c8799e9495aaeb193a7bade75d152269ce2f0588057c0e61fa6d4eaecf` | PE32 x86，2016-06-09 | API/版本交叉检查 |
| `origin/emotedriver.dll` | `774ff03f4e34cb9f99aae03613f0bb940b81d5c9eef285961266098f45b566c3` | PE32 x86，2015-06-25 | RTTI、driver ABI 与控制类型交叉检查 |

证据等级（后文规格使用固定英文标签，避免把推断写成事实）：

| 等级 | 本文写法 | 含义 |
|---|---|---|
| Confirmed | “确认” | 可由直接控制流、字段访问、RTTI/vtable、常量或调用参数逐指令复核 |
| Strongly inferred | “强推断” | 数据流和数值探针一致，但业务命名或边界意图仍由上下文推定 |
| Candidate | “候选” | clean-room 可采用的确定性策略，但不是原版已证明的意图 |
| Unknown | “未知” | 当前样本本身未定义，或仍缺调用/动态证据 |

静态分析能证明分支、字段、调用和状态变化，不能单独证明 Windows 原版在某个游戏场景中的最终像素。本文没有把 RTTI 名称、方法注册或“运行未崩溃”升级为行为兼容。

## 3. NCB 注册错位勘误

注册函数 `0x1007a330` 使用“一块延迟”：先构造 callback，在**下一段方法名**处才把前一个 callback 注册进去。若把字符串与其后紧邻的构造函数直接配对，整张方法表会错一格。

已复核的映射：

| TJS 成员 | callback 地址 | 关键证据 |
|---|---:|---|
| `progress` | `0x1005d5d0` | 将毫秒乘 60/1000，再调用 `frameProgress` |
| `frameProgress` | `0x1005d600` | 完整每帧控制/核心更新入口 |
| `draw` | `0x1005da50` | 下一注册块对应 draw |
| `initPhysics` | `0x1005e530` | 读取 metadata 控制表 |
| `startWind` | `0x1005dd10` | 五个浮点参数，构造 `EPWindControl` |
| `stopWind` | `0x1005de90` | 转调五零参数 `startWind` |
| `setOuterForce` | `0x1005bb70` | 3–5 参数与三类 label 分流 |

这条勘误很重要：旧的顺邻配对会把 `startWind` 当作 `initPhysics`，也会把 `getVariable` 错当 `setOuterForce`。后续地址映射必须先按延迟注册规则复核。

## 4. 总体数据流

```text
TJS / PSB metadata
  │
  ├─ initPhysics(metadata)
  │    ├─ bustControl  ─→ EPBustControl[]
  │    ├─ hairControl  ─→ EPPendControl[]
  │    ├─ partsControl ─→ EPPendControl[]
  │    └─ eyeControl   ─→ EPEyeControl[]
  │
  ├─ setOuterForce ─→ bust/hair/parts 插值器
  ├─ startWind     ─→ EPWindControl[128 gust slots]
  └─ *Scale        ─→ 三个持久幅度值
                         │
progress(ms) ─60/1000→ frameProgress(dt_frames)
                         │
                         ├─ 推进 timeline / 通用插值器
                         ├─ 更新 eye 与其它控制器
                         ├─ 推进 wind gust 槽
                         ├─ 核心 motion / node propagation
                         ├─ 推进三类 outer-force 插值器
                         ├─ bust update(scale, force, dt)
                         └─ hair/parts update(scale, force, dt)
                                  │
                                  └─ 写入具名数值表
                                           │
                                  下一次 frameProgress transfer（Confirmed）
                                           │
                                  核心变量 → 节点传播 → draw
```

### 4.1 时间单位

`progress` (`0x1005d5d0`) 的计算可直接复核为：

```text
frame_dt = milliseconds * 60.0 / 1000.0
frameProgress(frame_dt)
```

常量地址 `0x10132990`、`0x10132998` 分别解码为 `60.0` 和 `1000.0`。这与 M2 公开样例和 FreeMote 接口的 1/60 frameCount 语义交叉一致。

## 5. `initPhysics(data.metadata)`

入口 `0x1005e530` 直接读取 metadata 的 `mirror`、`scale`、`variableList` 以及多个 control 数组，然后调用各自解析/构造函数：

| metadata 项 | setup | 对象/消费者 |
|---|---:|---|
| `bustControl` | `0x10063e30` | `EPBustControl`；每帧 `0x10064a90` |
| `hairControl` | `0x100677d0` | `EPPendControl`；每帧 `0x100671b0` |
| `partsControl` | `0x10067870` | `EPPendControl`；每帧 `0x100671b0` |
| `eyeControl` | `0x10067910` | `EPEyeControl`；每帧 `0x10069140` |

同一入口还初始化 eyebrow、mouth、transition、selector、loop、clamp、mirror、timeline 等控制表。本文先聚焦用户可见缺口最明显的四类。

### 5.1 Bust 控制

解析器 `0x10063cf0` 读取：

- `gravity`
- `spring`
- `friction`
- `scale_x`
- `scale_y`

随后分配 `0x50` 字节并调用构造函数 `0x100c39c0`；vtable 指向 RTTI `emote::EPBustControl` (`0x10132904`)。五个参数写入对象 `+0x04` 至 `+0x14`，其后字段保存跨帧运动状态。

### 5.2 Hair / Parts 摆动控制

二者共用解析器 `0x10065be0`，读取：

- `gravity`
- `friction_x` / `friction_y`
- `b_rate`
- `v_bound`
- `ud_eft`
- `bend_spd` / `bend_vol`
- `length`
- `scale_x` / `scale_y`

解析器分配 `0xb0` 字节并调用 `0x100c8050`；vtable 指向 `emote::EPPendControl` (`0x1013292c`)。对象保留到 `+0xac` 的动态字段，说明它不是从输入直接计算一次输出的无状态映射。

### 5.3 `EPBustControl` 完整数学（P0）

#### 对象布局

| 偏移 | 字段 | 证据等级 |
|---:|---|---|
| `+0x04/+0x08/+0x0c` | `gravity` / `spring` / `friction` | Confirmed |
| `+0x10/+0x14` | `scale_x` / `scale_y` | Confirmed |
| `+0x18` | 首次更新标志，构造值 1 | Confirmed |
| `+0x1c..+0x24` | anchor/current input 三维点 | Confirmed |
| `+0x28/+0x2c` | 首帧记录的 input-relative x/y offset | Confirmed |
| `+0x34..+0x3c` | 动态质点位置 `p` | Confirmed |
| `+0x40..+0x48` | 动态速度 `v` | Confirmed |
| `+0x4c` | y 输出 bias；构造函数漏初始化 | Confirmed（用途）；Unknown（意图/初值） |

构造路径先以 `HeapAlloc(heap, 0, 0x50)` 分配对象；flags 为 0，不会清零。`0x100c39c0` 没有写 `+0x4c`，而 `0x100c3ce0` 无条件读取它。因此这是样本自身的未定义初值，不应再写成“默认 0”。clean-room 实现若要确定性，`0.0` 只能标为 **Candidate policy**，并需原版 trace 校准。

#### 固定步更新

`0x100c3ce0` 的参数为 `(inputX,inputY,outX*,outY*,forceX,forceY,dt,globalScale,angleRad)`；`this` 在 `ESI`。定义：

```text
R(-a) = [[ cos(a),  sin(a)],
         [-sin(a),  cos(a)]]

down  = (sin(a), cos(a), 0)
force = R(-a) * (forceX, forceY)
soft(z) = atan(z * 1.15 * (pi / 80)) / (pi / 80)
```

逐指令等价的状态更新为：

```text
if first:
    first = false
    inputOffset = anchor.xy - input
else:
    anchor.xy = inputOffset + input

delta = anchor - p
v += (spring * dt) * delta
v += dt * (force.x, force.y, 0)
v += dt * gravity * down
v.x *= 1 - friction * dt
v.y *= 1 - friction * dt
v.z *= 1 - friction * dt
p += dt * v                         // semi-implicit Euler

d = globalScale * (anchor.xy - p.xy)
outX = soft(-scale_x * d.x)
outY = soft(scale_y * (-d.y - legacyBias_0x4c))
```

字段、符号、旋转矩阵、积分顺序和饱和常量均为 **Confirmed**。只有 `legacyBias_0x4c` 的初值为 **Unknown**。

#### 消费者分步

`0x10064a90` 首次以 `dt=0` 调用 helper 建立 input offset。正常帧把总 `dt` 拆成不超过 `1.1` frame 的子步，并在旧/新 shape 坐标之间按子步位置插值；循环终止使用 `1.1920928955078125e-7` 容差。最大步长、容差和多次调用为 **Confirmed**；“这样做是为稳定积分”的设计动机为 **Strongly inferred**。

### 5.4 `EPPendControl` 完整数学（P0）

#### 对象布局

| 偏移 | 字段 | 证据等级 |
|---:|---|---|
| `+0x04` | `gravity` | Confirmed |
| `+0x08/+0x0c` | `friction_x` / `friction_y` | Confirmed |
| `+0x10/+0x14` | `b_rate` / `v_bound` | Confirmed |
| `+0x18` | `ud_eft`，选择第 0 或第 1 段生成第三输出；不是 bool | Confirmed |
| `+0x1c/+0x20` | `length[2]` | Confirmed |
| `+0x24/+0x28` | `scale_x[2]` | Confirmed |
| `+0x2c/+0x30` | `scale_y[2]` | Confirmed |
| `+0x34/+0x38` | `bend_spd` / `bend_vol` | Confirmed |
| `+0x3c` | 首次更新标志 | Confirmed |
| `+0x40..+0x48` | anchor | Confirmed |
| `+0x4c/+0x50` | input-relative offset | Confirmed |
| `+0x58..+0x6c` | 两段竖直 rest point | Confirmed |
| `+0x70..+0x84` | 两个动态点 `p[2]` | Confirmed |
| `+0x88..+0x9c` | 两个速度 `v[2]` | Confirmed |
| `+0xa0` | 第三输出的 y reference；构造函数漏初始化 | Confirmed（用途）；Unknown（意图/初值） |
| `+0xa4/+0xa8` | bend phase / envelope | Confirmed |
| `+0xac` | `EPWindControl*` | Confirmed |

和 Bust 一样，分配是 `HeapAlloc(..., flags=0)`。构造函数初始化 `+0xa4/+0xa8`，却跳过 `+0xa0`；正常输出无条件读取 `+0xa0`。确定性实现可候选设为 0，但不能声称这是 2016 原版定义的默认值。

#### 双段更新与约束

`0x100c86c0` 参数为 `(inputX,inputY,out0*,out1*,out2*,forceX,forceY,dt,globalScale,angleRad)`；`this` 在 `EBX`。输入锚点同步与 Bust 相同。每次调用先计算：

```text
rest[0] = anchor + (0, length[0], 0)
rest[1] = rest[0] + (0, length[1], 0)
down    = (sin(angle), cos(angle), 0)
force   = R(-angle) * (forceX, forceY)
```

随后按 `i=0,1` 更新。`base = anchor`（第 0 段）或 `p[0]`（第 1 段），`d = base - p[i]`，`r = length(d)`：

```text
if r > length[i] and r >= 0.015625:
    n = d / r
    stretch = r - length[i]
    if i == 0:
        v[0] += n * (stretch * b_rate * dt)
    else:
        p[1] += n * stretch             // 硬投影回长度边界
        radial = dot(v[1], n)
        v[1] += n * (-v_bound * radial * dt)

v[i] += dt * (force.x, force.y, 0)
v[i] += dt * gravity * down
if wind != null:
    v[i].x += wind.sample(p[i].x)        // 此项没有额外乘 dt
v[i].x *= 1 - friction_x * dt
v[i].y *= 1 - friction_y * dt
p[i] += dt * v[i]
```

约束分支、`0.015625` guard、第一段软回复、第二段位置硬投影/径向速度修正、风仅入 x、摩擦与积分顺序均为 **Confirmed**。

#### 三路输出与 bend

```text
dx = rest[i].x - p[i].x
dy = rest[i].y - p[i].y
out[i] = soft(-dx * scale_x[i] * globalScale)

if i == ud_eft:
    out2 = soft((yReference_0xa0 - dy) * scale_y[i] * globalScale)
```

helper 返回后，`0x100c9110` 以第三输出驱动 bend：

```text
if abs(out2) > 28:
    envelope = min(1, envelope + dt / 32)
else:
    envelope = max(0, envelope - dt / 32)

phase = fmod(phase + bend_spd * envelope, 2*pi)
bend = sin(phase) * envelope * bend_vol
out0 -= bend
out1 += bend
```

阈值在 `abs(out2) == 28` 时走衰减分支；phase 增量本身不再乘 dt。这些均为 **Confirmed**。hair/parts 消费者采用与 Bust 相同的 `1.1` 最大子步与坐标插值策略。

### 5.5 当前实现差距

2026-08-15 当前实现已用事务 candidate 解析并拥有这些 control；下列项目现由 `PlayerPhysics.cpp` 与 `MotionPhysics.cpp` 实现。这里保留为研究到实现的检查边界：

1. 完整解析并拥有各 control 对象；
2. 明确重载/切换素材时的销毁与重建；
3. 在 `frameProgress` 中按原顺序推进；
4. 把控制输出接入当前变量/节点求值，而不只是设置 dirty flag。

## 6. `eyeControl` 与自动眨眼

### 6.1 元数据字段与对象

`eyeControl` 解析路径确认读取：

| 字段 | 已恢复用途 |
|---|---|
| `enabled` | 控制项启用 |
| `beginFrame` | 睁眼/起始帧 |
| `endFrame` | 闭眼/目标帧 |
| `blinkIntervalMin` | 随机眨眼间隔下界 |
| `blinkIntervalMax` | 随机眨眼间隔上界 |
| `blinkFrameCount` | 闭合/张开的推进时长尺度 |
| `blinkEnabled` | 自动眨眼开关 |
| `edge` | 输入到帧值的图/边结构 |
| `node` | 输出绑定目标 |

构造函数 `0x100c5710` 分配/初始化 `0xb4` 字节对象，vtable 指向 `emote::EPEyeControl` (`0x10132914`)。关键运行字段包括：

| 偏移 | 初值/含义 |
|---:|---|
| `+0x6c` | 手动控制 mode，初值 0 |
| `+0x70` | 当前基础输出，初值 `beginFrame` |
| `+0x74/+0x78` | 插值状态，初值 0 |
| `+0x88` | blink phase，初值 0 |
| `+0x8c` | 当前 blink frame，初值 `beginFrame` |
| `+0x90` | 随机间隔倒计时 |
| `+0x94` | blink enabled |
| `+0x98` | 抑制/override flag，初值 0 |

### 6.2 状态机

每帧函数 `0x100c5a60(dt)` 是 0–12 共 13 个 case 的状态机。0、10、11、12 构成自动眨眼闭环；1–9 处理手动/edge 图驱动的插值和覆盖。

自动眨眼可写成以下高置信伪代码：

```text
state 0: idle
  if blinkEnabled && !suppressed && current == beginFrame:
    countdown -= dt
    if countdown <= 0:
      phase = 10

state 10: closing
  current += (endFrame - beginFrame) * dt / blinkFrameCount
  if current reaches endFrame:
    current = endFrame
    hold = blinkFrameCount / 5
    phase = 11

state 11: closed hold
  hold -= dt
  if hold <= 0:
    countdown = random(blinkIntervalMin, blinkIntervalMax)
    phase = 12

state 12: opening
  current -= (endFrame - beginFrame) * dt / blinkFrameCount
  if current reaches beginFrame:
    current = beginFrame
    phase = 0
```

边界比较同时处理 `beginFrame > endFrame` 的方向；上面为常见正方向的可读表达。精确 RNG 到浮点区间的端点语义还需动态或更细指令级复核。

### 6.3 输出链

`frameProgress` 调用 `0x10069140`，后者遍历 eye 对象并调用 `0x100c5a60(dt)`。返回值结合 `edge`/`node` 绑定写入 wrapper 的具名数值表（map helper `0x1007cfb0`）。这证明自动眨眼不是仅更新私有计数器；它生成供核心变量/节点系统消费的数值。

当前实现已经具有持久自动 blink 主循环，并在本次 Core 求值前写入变量表。状态 1–9 的完整 edge/manual 图语义仍未恢复；现代实现用“非 beginFrame 的外部值抑制自动 blink”保护手动路径，不把它宣称为原版完整行为。

## 7. `setOuterForce(name,x,y,time,easing)`

入口 `0x1005bb70` 接受 3–5 个参数；dispatch helper `0x1005be20` 明确识别：

| name | wrapper 控制器字段 |
|---|---:|
| `bust` | `+0x1b8` |
| `hair` | `+0x1bc` |
| `parts` | `+0x1c0` |

每一路把 `x`、`y`、`time`、`easing` 和 wrapper `+0x30` 的 mode byte 传给 `0x100c9f30`，配置持久二维插值器。旧文档把该 byte 直接称为 mirror；指令只确认它选择“追加队列”或“清队列后替换”，没有直接对 x/y 取反，因此业务名降级为 **Unknown**。

可确认的契约：

```text
setOuterForce(name, x, y, time = 0, easing = 0)
```

- 省略 `time/easing` 时外部默认分别为 `0` / `0`；内部把 easing 0 归一化为指数 1；
- label 是 `bust` / `hair` / `parts`；
- wrapper mode byte 决定追加还是替换，不改变目标坐标本身；
- 三类力分别进入 bust、hair、parts 的每帧消费者。

当前实现已将 `hair` 作为 canonical label，并保留 `h` 兼容别名；三路状态在 post-Core physics 阶段分别被 Bust/Hair/Parts consumer 消费。

### 7.1 easing 归一化与插值（P1）

入口 `0x1005bf90` 将脚本 easing `e` 转为幂指数 `k`：

```text
if e > 0: k = e + 1
if e < 0: k = 1 / (1 - e)
if e == 0: k = 1
```

因此负值指数落在 `(0,1)`，形成快起慢收；正值指数大于 1，形成慢起快收；0 为线性。这与公开 manual 的“负 ease-in / 正 ease-out”文字命名方向可能不同，视觉/命名仍应以原版 trace 为准，但数值曲线为 **Confirmed**。

`0x100c9f30`、`0x100ca2b0`、`0x100ca0f0` 和 `0x100ca070` 的职责已拆开：

| 地址 | 作用 | 证据等级 |
|---:|---|---|
| `0x100c9f30` | `time <= 0` 时清队列并立即复制目标；否则创建 24-byte transition record | Confirmed |
| `0x100ca2b0` | 把 record 放入环形队列；不计算 easing | Confirmed |
| `0x100ca0f0` | 取队首、推进 progress、做幂插值、完成时精确落到 target | Confirmed |
| `0x100ca070` | 将 controller 当前值数组复制给调用者；不计算 easing | Confirmed |

二维 outer-force 的核心可写成：

```text
on segment start:
    start = current
    target = record.xy
    invDuration = 1 / record.time
    exponent = record.normalizedEasing
    progress = 0

on update(dt):
    progress += invDuration * dt
    if progress >= 1:
        current = target
        pop segment
    else:
        weight = pow(progress, exponent)
        current = start + (target - start) * weight
```

完成比较使用一个极小浮点容差；最终值通过直接复制 target，避免累计误差。上述曲线、状态推进和队列职责均为 **Confirmed**。

## 8. `startWind` / `stopWind`

### 8.1 构造与参数规范化

`startWind` (`0x1005dd10`) 的五参和公开契约一致：

```text
startWind(start, goal, speed, powMin, powMax)
```

行为轮廓：

1. 若 `speed < 0`，交换 `start/goal` 并将 speed 取正；移动方向仍由区间方向编码。
2. 五个值全为零时销毁并清空 wrapper `+0x48` 的 wind 对象。
3. 否则分配 `0x620` 字节并调用构造函数 `0x100cb300`。
4. 对象 vtable 指向 `emote::EPWindControl` (`0x10132944`)。
5. `0x100cb460` 保存 `powMin/powMax`、激活标记、有符号速度和累计器。

`stopWind` (`0x1005de90`) 直接构造五个零值并调用 `startWind`，因此停止语义是销毁/清空，不是把速度设零后保留旧阵风。

### 8.2 128 槽阵风状态机

对象包含 128 个 12 字节 gust record。每帧 `0x100cb530`：

```text
accumulator += abs(speed) * dt
while accumulator crosses a whole unit:
  choose a free gust slot (maximum 128)
  gust.active = true
  gust.position = start
  gust.power = random(powMin, powMax)
  accumulator -= 1

for each active gust:
  gust.position += signedSpeed * dt
  if position reaches/passes goal:
    gust.active = false
```

随机源是进程内惰性初始化的 xorshift-like 全局状态。固定种子/注入 RNG 是未来确定性测试的必要条件。

### 8.3 风的真实空间查询（P1 勘误）

`EPWindControl` 的关键布局为：

| 偏移 | 字段 |
|---:|---|
| `+0x04/+0x08` | start / goal |
| `+0x0c` | active |
| `+0x10/+0x14` | powMin / powMax |
| `+0x18` | signedSpeed |
| `+0x1c` | spawn accumulator |
| `+0x20 + 12*i` | gust `{ active:u8, position:f32@+4, power:f32@+8 }` |

真正被 `EPPendControl` 调用的是 `0x100cb4b0(x)`：

```text
for gust in slots[0..<128]:
    if !gust.active: continue
    radius = 2 * gust.power
    if gust.position - radius <= x <= gust.position + radius:
        return gust.power * signedSpeed
return 0
```

它按槽序返回**第一个**命中值，不对重叠 gust 求和，也没有平滑 falloff。区间与返回式为 **Confirmed**；等号边界由 x87 比较形状判断为 **Strongly inferred**。Pend 将返回值直接加到每段 `v.x`，不额外乘本次 `dt`。

`0x100cb370` 则为每个 active gust 生成两份 20-byte 可视化顶点数据；`0x100c9200` 只是对应 vector 的扩容/复制 helper。这里使用的 `2.5 * abs(signedSpeed)` 几何宽度属于调试/可视化，不是物理空间核。旧版“`0x100cb370` 汇入力场”的结论已撤回。

当前 `Player::startWind` 把五参命名/解释为角度、振幅、频率，并只存单份状态；这与 2016 原版的区间内多阵风生成模型不一致。

## 9. 三个 scale 属性

延迟注册规则校正后，setter 与 wrapper 字段为：

| 属性 | setter | wrapper 字段 | 每帧消费者 |
|---|---:|---:|---|
| `bustScale` | `0x1005df10` | `+0x18` | `0x10064a90` |
| `hairScale` | `0x1005deb0` | `+0x20` | `0x100671b0` hair 路径 |
| `partsScale` | `0x1005dee0` | `+0x28` | `0x100671b0` parts 路径 |

`frameProgress` 对 hair/parts 两次调用同一个摆动消费者，但分别传入不同对象集合、scale 和 outer-force controller；bust 使用独立消费者。由此可确认 scale 是物理响应幅度/输入的一部分，而不是纯 getter/setter 状态。

当前实现虽保存三个属性，但没有对应 control 对象和消费者，所以不会形成原版的可见物理差异。

## 10. `frameProgress` 中的完整顺序

`0x1005d600` 的高层调用顺序如下：

1. 推进 timeline 和通用插值器；
2. 更新 eye、eyebrow、mouth、transition、selector 等控制器；
3. 若 wind 存在且激活，调用 `0x100cb530(dt)`；
4. 调用 `0x10074260`，把上一轮留在 wrapper 数值表的值转入核心变量；
5. 调用核心更新 `0x10028590(dt)`、`0x1002ea70()`、`0x10036b30()`、`0x10021440(0)`；
6. 当 physics 未禁用且 `dt != 0`：
   - `0x10063600(dt)` 推进 bust/hair/parts outer-force 插值器；
   - `0x10064a90(wrapper,dt)` 更新 bust，内部消费 bust force 和 `bustScale`；
   - `0x100671b0(...,hairScale,dt,hairForce)` 更新 hair；
   - `0x100671b0(...,partsScale,dt,partsForce)` 更新 parts；
7. bust 写出两个具名值，hair/parts 各写出三个具名值到 wrapper 数值表。

数值表 helper `0x1007cfb0` 按 TJS 字符串查找/插入并返回 double 槽；调用者立即写入计算结果。另一路 `0x10074260` 遍历该表，处理 mirror 后调用核心变量 setter `0x1001c620`。核心随后通过 `0x10036b30` 做节点传播并进入 draw。

`0x10074260` 在该 DLL 中只有 `frameProgress` 这一处调用；本函数在 physics 写表后没有第二次 transfer，也没有第二次 `0x10036b30` 节点传播。因此调用 `frameProgress(n)` 产生的 physics 值，最早在 `frameProgress(n+1)` 的 transfer → core update → node propagation 中生效。这一帧 staging 对该样本是 **Confirmed**。

独立 `draw` callback 在 `frameProgress` 之后绘制已经传播好的核心状态；它不会重新转移 wrapper 表，因此不能消除延迟。宿主若额外调用一次 `frameProgress`，呈现帧号当然会改变；这属于宿主调度，不能改写 DLL 内部顺序。

### 已闭合与未闭合的链

| 链段 | 状态 |
|---|---|
| TJS 参数 → wrapper 分流 | 已闭合 |
| metadata 字段 → control 对象 | 已闭合 |
| control/force/wind/scale → 每帧消费者 | 已闭合 |
| 消费者 → 具名数值表 | 已闭合 |
| 数值表 → 核心变量 setter → 节点传播 | 已闭合 |
| 节点传播中的精确变量映射/数学 | 部分闭合 |
| 最终 mesh 顶点和像素与原版逐帧一致 | 未验证 |

## 11. 动态 oracle 评估

当前主机没有 `wine`/`wine64`，且样本是 32 位 Windows/Kirikiri 插件，不能在 macOS 进程中直接加载完整插件。因而本轮没有执行 V2Link、TJS、renderer 或真实游戏；这是环境边界，不是“原版运行通过”。

研究脚本 `research/emulate_control_helpers.py` 只映射 PE 并进入无 Windows API 依赖的构造/数学 helper。它先用 `0xA5` poison 证明 `+0x4c/+0xa0` 未被构造函数触碰，再显式注入基准值做数值交叉检查。该探针支持指令分析，但不提升为插件或游戏兼容性证据。

字段差分进一步确认：Bust `+0x4c` 从 `0` 改为 `7.5` 时首帧 `outY` 从约 `-0.0252273` 改为 `-7.55771`，位置和速度不变；Pend `+0xa0` 从 `0` 改为 `-3.25` 时首帧 `out2` 从约 `-0.0491601` 改为 `-3.30086`，两段位置、速度和 bend 状态不变。当前 C++ 将二者集中固定为 `0.0`，只为避免继承未定义堆内容；这是 compatibility policy，不是原版默认值。

可用的下一步 oracle 分三层：

1. **纯状态模型**：按本文状态机实现可注入时钟/RNG 的独立测试，先验证 idle-close-hold-open 和 128 槽生命周期；它验证我们的理解，不验证原版动态输出。
2. **Windows 原版 harness**：在匹配 32 位 KiriKiri/插件环境中，用最小 PSB 和 TJS 记录具名变量/帧输出；这是校准默认值、easing、RNG 和一帧延迟的最佳 oracle。
3. **最终像素对照**：同一素材、初始状态、固定时间序列和随机种子，对比原版与当前 runtime 的节点/表面输出；这是兼容性结论所需证据。

FreeMote WebGL 可以作为第三方 Pure PSB oracle，但版本、wrapper 和核心都不是 2016 DLL，不能代替第二层。

## 12. 实现难度与建议顺序

| 工作项 | 难度 | 逆向需求 | 原因 |
|---|---|---|---|
| 修正 `hair` label、五参命名和参数边界 | 低 | 已足够 | 契约和 2016 dispatch 已明确 |
| `eyeControl` 解析与自动 blink 主循环 | 中 | 主循环已足够；1–9 状态仍需补 | 可先做固定 RNG 的独立状态机，但完整手动覆盖需继续研究 |
| outer-force 持久插值与三路挂接 | 中高 | 数学已足够 | 幂 easing、队列、立即值和消费者已闭合；仍需真实节点验收 |
| wind 128 槽发生器 | 中高 | 数学已足够 | 生命周期和第一命中矩形核已闭合；RNG 可重复性仍需策略 |
| bust/hair/parts 完整物理 | 高 | 主数学已足够 | 积分、约束、输出、bend 已闭合；两个原版未初始化字段需兼容策略 |
| 原版逐帧/像素等价 | 很高 | 必须有动态 oracle | 还涉及核心变量映射、节点求值、renderer 与版本差异 |

建议顺序：

1. 先建立可注入 RNG/clock 的控制基础设施和纯状态测试；
2. 实现 `eyeControl` 主 blink 循环，以具名变量和眼部节点结果验收；
3. 修正 outer-force label/参数并按已恢复幂曲线实现插值器；
4. 实现 wind 生命周期和第一命中区间查询；
5. 在明确 `+0x4c/+0xa0` 的确定性策略后移植 bust/pendulum，并接入当前节点求值；
6. 每一步都用真实 `Plugins.link` TJS 调用和最终节点/像素补证，不能只测 C++ 对象字段。

## 13. 可复核命令

以下命令不执行 DLL，只复核样本身份、注册入口和指定函数反汇编：

```zsh
file cpp/plugins/motionplayer/origin/*.dll
shasum -a 256 cpp/plugins/motionplayer/origin/*.dll

radare2 -q -e scr.color=false \
  -c 'aaa; af @ 0x1005d5d0; pdf @ 0x1005d5d0' \
  cpp/plugins/motionplayer/origin/emoteplayer.dll

radare2 -q -e scr.color=false \
  -c 'aaa; pdf @ 0x1005e530; pdf @ 0x1005bb70; pdf @ 0x1005dd10; pdf @ 0x1005d600' \
  cpp/plugins/motionplayer/origin/emoteplayer.dll

PYTHONPATH=/tmp/motionplayer-unicorn python3 \
  cpp/plugins/motionplayer/research/emulate_control_helpers.py \
  cpp/plugins/motionplayer/origin/emoteplayer.dll bust --frames 4
```

完整反汇编很长；复核时应同时查看调用者和被调用者，不要只凭反编译器自动函数名。若启用 reloc apply 或换用其它工具，地址显示可能变化，应以样本 SHA-256 和 RVA 对照。

## 14. 尚未证明的事项

- 没有证明 `motionplayer.dll`、`emoteplayer.dll` 与所有 NEKOPARA 版本共享同一算法。
- 没有恢复 `EPEyeControl` 状态 1–9 的完整业务名称和所有 edge 图语义。
- `EPBustControl +0x4c` 与 `EPPendControl +0xa0` 的业务意图/初值未知；样本本身读取未初始化 HeapAlloc 内容，无法从该二进制恢复一个不存在的确定默认值。
- wrapper `+0x30` mode byte 的业务名称未知；只确认它改变 transition queue 的追加/替换策略。
- wind 查询的浮点等号边界还应由原版动态 trace 补证，但主体区间与返回公式已闭合。
- 没有执行 2016 原版 DLL，也没有原版节点 trace 或逐帧像素 oracle。
- 没有证明当前 renderer 已具备承接这些控制输出所需的所有 mesh/deform 语义。

这些缺口不会推翻已闭合的 API/状态/消费者链，但会阻止“完整复刻”或“NEKOPARA 已兼容”的结论。
