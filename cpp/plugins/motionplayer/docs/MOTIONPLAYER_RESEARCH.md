# MotionPlayer / EmotePlayer 当前研究基线

> **现状权威入口。** 本文件只记录能够由当前源码、可复现检查或明确标注的游戏观察支持的结论。专题文档是历史调查材料；与本文件或当前源码冲突时，以当前源码为准。
>
> **重要前提：** 原版 `motionplayer.dll` 与 `emoteplayer.dll` 均为闭源商业插件。本仓库没有已验证的原版源码，也没有获得原版 ABI/行为的完整规范。
>
> 代码路径：`cpp/plugins/motionplayer/`
>
> 文档索引：[README.md](README.md)
>
> 三方接口逐项对照：[MOTIONPLAYER_API_CONTRACT_MATRIX.md](MOTIONPLAYER_API_CONTRACT_MATRIX.md)
>
> 2016 DLL 控制行为静态研究：[MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md](MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md)
>
> 最后核对：2026-08-17

---

## 1. 问题范围

当前实现并不等价于原版 MotionPlayer / EmotePlayer。依赖简单移动、缩放或切换的脚本可能看起来正常；这不能外推为完整兼容。重度使用 E-mote 的游戏会同时触发控制器、Timeline 混合、物理、风和复杂渲染路径，因此更容易暴露缺口。

NEKOPARA 是当前最重要的重度样本：用户观察到眨眼、物理、姿态、表情/风和渲染异常。该观察可以用于确定调查优先级，但在建立可重复的游戏矩阵前，不应写成所有版本、角色或场景都必然出现的统一结论。过去对脸、眼、眉、鼻、嘴的窄场景验证，也只证明对应素材与流程，不证明完整 E-mote 兼容。

## 2. 证据与权威顺序

| 等级 | 资料 | 可以证明什么 | 不能证明什么 |
|---|---|---|---|
| 1 | 当前 `cpp/plugins/motionplayer/` 源码与注册表 | 本仓库现在解析、更新和暴露了什么 | 与闭源原版等价、商业游戏可用 |
| 2 | 当前可重复测试、插件安全扫描、真实 `Plugins.link` 成员调用 | 被检查路径的具体行为 | 未覆盖的动画、Release runtime 或性能 |
| 3 | 带版本与场景的真实游戏观察/trace | 特定样本上的可见结果 | 其它版本、角色或游戏的普遍结论 |
| 4 | M2 公开 KiriKiri Sample SDK 的 `manual.tjs`、示例脚本与 DLL 元数据 | 公开样例版本的 TJS API 形状、时间单位和调用链 | 闭源算法、NEKOPARA 所用版本的行为 |
| 5 | FreeMote 公共头文件/样例、导出的 PSB/JSON、历史 `sdl3-ref/`、具名二进制静态分析 | 独立交叉验证；静态分析可证明样本中的控制流、字段访问和状态变化 | 动态最终像素、跨版本等价或可直接移植性 |
| 6 | 旧文档、注释、TODO、历史日志 | 研究过程和待验证假设 | 当前能力或修复完成状态 |

本地 `manual.tjs` 的 API 声明已与 M2 公开的 2020 KiriKiri Sample SDK 对照；除示例末尾两个拼写修正和文本编码/换行外，相关接口一致。它可以证明该公开样例版本的接口形状，但闭源行为仍须由脚本调用、二进制、trace 与可见结果交叉验证。

## 3. 当前实现审计

| 能力 | 当前状态 | 当前源码证据 | 结论边界 |
|---|---|---|---|
| PSB/MTN 加载、节点树与基本渲染 | 已有实现 | `PlayerMotionLoad.cpp`、`NodeTree.cpp`、`PlayerFrameProgress.cpp`、绘制相关文件 | 尚无当前功能质量门证明完整格式与真实游戏兼容 |
| `transformOrder` | **已实现** | `NodeTree.cpp` 解析；`PlayerUpdateLayersInternal.h` 和 `PlayerUpdateLayerEval.cpp` 应用 | 不能再列为“未实现”；具体姿态问题仍需逐样本 trace |
| Timeline blend ratio | **已实现全参契约** | `PlayerTimeline.cpp` `setTimelineBlendRatioEx` → `setTimelineBlendLike_0x6735AC` 动画器；注册 raw callback 接受 2–5 参 | 游戏实际以 `(name, ratio, time*60/1000, easing)` 调用（time 为帧）；此前 2 参注册静默丢弃 time/easing 导致 diff 时间线瞬时弹出，2026-08-16 修复，待游戏内验证 |
| Timeline 启动语义 | **已修复 play() 全量误启动** | `PlayerMotionLoad.cpp` / `PlayerTimeline.cpp`（`playMotionLike_0x6B2284`、`playCompat`） | `play(metadata.base.motion, Force)` 的 label 是 clip 名（如“全体構造”）而非 timeline label；此前当 label 不在 timeline 表中时兜底启动**全部** main timeline，所有控制轨道（`sample_全自動_test`、`sample_00..05`、`変な動き`、`ピョン`…）同时驱动同一批姿势变量，正是“每次切换固定做一套诡异动作（身体晃→头晃→抽搐）”的根因。2026-08-16 改为只启动与 label 同名的 timeline，否则不启动；探针验证 play 后 0 次控制帧跨越、`animating` 空闲为 false、pose timeline 播完回 false（原版 sync 边沿流程）。另修复 `resetTimelineControlStateLike_0x671A50` seek 时把首个未来关键帧值当作当前值应用的 off-by-one（break 前更新 `lastNonTypeZero`） |
| Timeline fade | **参数语义不对齐** | `EmotePlayer.cpp` 当前第三参作为整数 flags | 公开 KiriKiri 样例第三参是 easing；需要兼容回调和行为测试 |
| `eyeControl` / 自动眨眼 | **已实现确认的自动主循环** | `MotionPhysics.cpp`、`PlayerPhysics.cpp` | pre-Core 同调用写表；状态 1–9 的完整 edge/manual 语义仍未恢复，手动非初值会抑制自动输出 |
| `initPhysics` | **事务化实现** | `PlayerPhysics.cpp` | candidate 完整解析后保留非空 `baseLayer` 字段并 swap；根/嵌套 motion 与 particle player 的层在首次正 `dt` physics consumer 解析，避免 child clip 尚未 materialize 时误杀整张立绘；素材 generation 不匹配时旧状态休眠 |
| Bust/Hair/Parts physics | **已接入 post-Core staging** | `MotionPhysics.cpp`、`PlayerPhysics.cpp` | 数值内核对固定 2016 DLL helper golden；最终节点/像素与商业游戏仍未验证 |
| `startWind` | **已实现 128 槽生命周期与 Pend 消费** | `MotionPhysics.cpp`、`PlayerPhysics.cpp` | 生产使用 Player RNG；精确跨版本随机序列未证明 |
| `setOuterForce` | **已实现三路插值与消费** | `MotionPhysics.cpp`、`PlayerPhysics.cpp` | canonical `hair`，保留 `h` 别名；公开 API 不变 |
| accurate-SLA 零件层身份 | **已改用 `layerId` 作复用键** | `PlayerRenderTargets.cpp` `renderAccurateSlaLike_0x6C9CA8` | 合并子 motion/particle 项携带子 Player 的 nodeIndex，与父 nodeIndex 冲突会互相驱逐层节点（NEKOPARA 头部/面部子树即此路径）；`layerId1` 由整树共享的 ResourceManager 分配，全局唯一且按素材稳定。真实游戏像素验证仍待进行 |
| accurate-SLA 层序写入 | **已改为变化时写入** | `SeparateLayerAdaptor.cpp` `resolveLayerNodeLike_0x6C6B48` | 每帧无条件重写 `absolute` 会在顺序未变时反复触发父层 exposed-region 失效；现在只在新建层、序位变化或 `_absolute` 基址变化时重插。多角色/复杂场景的帧开销仍需真实采样 |
| 触摸包围盒 | **已改为渲染 AABB + last-good** | `PlayerRenderItems.cpp` `calcBounds`；`EmotePlayer::contains`；`Player::hitTestBounds` | emote 不再跳过 head/face/body 子树；空帧保留上一份有效盒。游戏脚本若另做逆仿射，仍需逐样本确认 |
| 立绘栅格质量 | **已改为双线性 + authored 网格** | `PlayerUpdateGeometry.cpp`、`PlayerRenderTargets.cpp`、`SourceCache.cpp` | `stFastLinear` / `GL_LINEAR`；网格帽 20；五官/parameterize 层不再压成 2×2。动作帧成本会上升，需再采 `sla.accurate.stats` |
| 同 PSB 五官 clip 短名 | **已实现 `鼻`→`鼻(...)`** | `PlayerMotionLoad.cpp`、`Player::selectActiveClip` | 只接受括号变体，不把 `口` 绑到 `口パク`。跨 PSB 且未 attach 的引用仍会失败 |
| API 注册兼容 | **需逐项审计** | `main.cpp` / NCB 注册与各 Player 方法 | 尤其检查 Timeline fade/blend、wind、physics 的参数个数、单位与失败语义 |

这张表描述“代码里有什么”，不是 bug 根因表。比如姿态错乱不能再简单归因于 `transformOrder` 缺失，因为该字段已经进入当前矩阵更新路径。

## 4. 研究资料的正确定位

### 4.1 `sdl3-ref/`

当前归档内容与历史提交 `c16210f` 中对应文件一致。它是本项目历史分支的实验性快照，不是 M2/原版插件源码，也不是当前 runtime：

- `EmoteFileCore.cpp` 有眨眼流程轮廓，但每次调用重新构造默认随机引擎；只能作为状态机线索。
- `EmotePhysics.cpp` 自述为 AI 编写且包含演示/硬编码风逻辑；`emoteplayerclass.cpp::initPhysics` 仍是 TODO。它不能称为“完整物理实现”，也不能作为直接移植基线。
- 旧 `emotefile` 架构与当前 `Player` 管线不同；旧文件名、调用链和测试结果不得直接套到当前代码。

### 4.2 Windows DLL 与 NEKOPARA 归档

`origin/` 是未跟踪的本地研究输入，不进入 Git。已确认三枚 32 位 PE DLL 与三个 XP3：

- `emoteplayer.dll` / `motionplayer.dll` 只导出 `V2Link` / `V2Unlink`，但保留 NCB 模板、RTTI、API 字符串和 PDB 路径；`emotedriver.dll` 还导出 `IEmoteDevice`、`IEmotePlayer`、`EmoteCreate` 与 `EmoteFilterTexture`。
- 2016 `emoteplayer.dll` 不只保留名称：已静态恢复 metadata parser、`EPEyeControl` 自动眨眼、Bust/Pend 双段物理、wind 第一命中区间查询、outer-force 幂 easing、scale 消费者、具名数值写回与下一帧 staging。详见[闭源行为研究](MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md)。这些指令级结论仍不证明完整插件动态像素或跨版本等价；样本还含两个被正常读取的未初始化物理字段，clean-room 实现必须另定确定性策略。
- `emote.xp3` 含 79 个 PSB 与 8 个 UTF-16LE `.mstand`；`emotewin.xp3` 与 `emotewin_low.xp3` 各含 39 个 `dx_` / `dxlow_` PSB。三组素材可用于区分基础数据、Windows/DX 高低规格纹理与动画逻辑问题。
- XP3 中的 `バニラパジャマa.psb` 与当前 fixture 大小和 SHA-256 均不同；现有 fixture 不能冒充生产资产的逐字节副本。

### 4.3 Android `libkrkr2.so` 样本

已检查的 arm64、stripped 本地研究样本 SHA-256 为：

```text
ded611b9018cfca425e97d5f8aaaa5dff809c4bacefb66ba77806372ddb52b38
```

样本只发现 `MMotionPlayer` 字符串，没有可用的 C++ 类符号。静态反汇编可支持五个浮点值被保存为 wind 状态这一窄结论；它不能证明 `initPhysics`、自动眨眼或 Spring 物理存在。该样本不是仓库构建输入，也不应写入用户本地路径。

### 4.4 M2 公开 KiriKiri Sample SDK

[M2 Sample SDK 下载页](https://emote.mtwo.co.jp/download/sdk/) 当前公开 `E-mote SDK Sample for KIRIKIRI.zip`。2026-08-14 下载样本 SHA-256：

```text
7b5002ad122d24c40a2b1ab086e34c1d4ee0bcef127b98cf1e1ab53089514c6e
```

包内 2020-06-08 的 32 位 `emoteplayer.dll` 与 NEKOPARA `origin/` 中 2016 DLL 不是同一构建。公开样例还包含 `manual.tjs`、`data/scenario/emoteplayer.ks` 与两份 PSB v3 样本，提供以下可复核契约：

- TJS `progress(diffTick)` 使用毫秒；底层 E-mote driver 使用 1/60 秒 frameCount。
- `initPhysics(data.metadata)` 在加载/切换素材后调用。
- `setOuterForce(name, x, y, time, easing)`、`startWind(start, goal, speed, powMin, powMax)`。
- `setTimelineBlendRatio(name, ratio, time, easing, stopWhenBlendDone)`；fade 的第三参是 easing。
- 前后页 Player 分离、serialize/unserialize、Timeline 主/差分 flags 和每 tick 的 progress/draw 顺序。

这些事实只属于公开样例版本；NEKOPARA 2016 DLL 的精确行为仍要单独验证。

### 4.5 FreeMote-SDK

[Project-AZUSA/FreeMote-SDK](https://github.com/Project-AZUSA/FreeMote-SDK) 当前 HEAD 为 `4340e8fc2138f013157d5b9cd40ba17d01347093`（最后源码提交 2020-05-24）。README 标注 v3.82、Windows DX9 与 WebGL 可用、KiriKiri 未实现。

- `Windows-DX9/iemote.h` 给出完整 `IEmoteDevice` / `IEmotePlayer` 虚接口，与 M2 公开样例和 DLL 类型线索交叉印证 wind、outer force、Timeline 与 1/60 frameCount 契约。
- WebGL wrapper 展示 `hair` / `parts` / `bust` 三类 outer force、wind 的 `(-width/2, width/2, speed, powMin, powMax)` 调用，以及毫秒到 frameCount 的转换。
- 核心引擎位于约 1.25 MB 的编译后 `FreeMoteDriver.js`，不是可读的物理/渲染 C++ 源码；它更适合作为 Pure PSB 的第二行为 oracle。
- wrapper 存在明显错误，例如 `getTimelineBlendRatio()` 错调 total-frame-count getter；不能整段照搬，结论须回到接口与实测。

### 4.6 本地样本数据

- `manual.tjs`：已确认来自 M2 公开 KiriKiri 样例接口文本的规范化/拼写修正版；可证明公开样例 API，不证明闭源内部算法。
- `tests/test_files/emote/*.json`：从素材导出的结构样本，不是 runtime 文件格式，也不是格式规范。
- `tests/test_files/emote/*.psb`：E-mote v2 容器，header 之后的 names/strings 区域用与 NEKOPARA 相同的 keystream（seed 742877301）加密；`PSBFile` 在 `_seed>0 && version==2` 时对 `[offsetEncrypt, offsetChunkOffsets)` 原位解密。2026-08-16 本地 runtime 实测生产 PSB 与 fixture 均带种子 <100ms 解析成功；不带种子在 stringOffsets 处报 `PSBArray bad length type size`。motionplayer smoke fixture 因此必须先调用 `setEmotePSBDecryptSeed`。
- 游戏脚本与运行观察：应记录游戏/版本、场景、调用参数、运行时 commit 和可见结果；只保留必要片段，不提交游戏原文件。

## 5. 已退役的旧结论

以下说法不得继续作为当前事实引用：

- “krkrz 官方仓库有 win32 motionplayer 原版源码”。没有找到可验证的对应源码。
- “`sdl3-ref` 是原作者完整旧实现”或“包含完整头发物理”。它是本项目历史实验代码，物理文件明确带有 AI/演示性质。
- “Android so 含 `MMotionPlayer` 类符号，可直接逆向完整眨眼/物理”。当前只确认字符串与一段 wind 状态写入行为。
- “`transformOrder` 尚未实现”。当前源码已经解析并应用。
- “旧 `motionplayer-dll.cpp`、`motionplayer-render.cpp`、`tests/test_files/render/run.sh` 回归仍然代表当前实现”。这些历史路径不存在；当前重新建立的是 `motionplayer-physics` focused test、独立 golden 和新的 quality-gate fixture。
- “轻度使用 motionplayer 的游戏都没有问题”或“NEKOPARA 所有症状都已稳定复现”。没有覆盖足够版本和场景的矩阵，不作普遍断言。

## 6. 当前验证基线

2026-08-15 对 `cpp/plugins/motionplayer`、`cpp/plugins/EmotePlayer` 与公共插件边界运行安全扫描，结果为 62 个源文件、0 个阻断项。纯数值 focused test 为 11 个 test case、96 个 assertion；Bust/Pend fixture 由固定 SHA-256 的 2016 DLL 独立 Unicorn harness 生成。工程 CMake/quality gate 曾受本机 vcpkg 工具链故障阻断（checkout 丢失 `.git` 与 osx triplets）；2026-08-16 用锁定 baseline `aa40adda5352e87655b8583cfb2451d5e9e276fd` 的完整 clone 修复后，`--plugin motionplayer` 质量门全绿（scanner、debug/release 构建与 ctest、双 smoke、release anchor）。渲染层另有两条已实现的路径级修复（`layerId` 复用键与层序变化时写入），smoke fixture 现含两次 SLA draw 的渲染路径覆盖（fixture 带 382 个嵌套 type-3 子 motion）。

2026-08-16 本机可重复探针（fixture PSB 经 accurate-SLA 软件路径，1280×720 canvas）：

- 单立绘：首帧 draw 49ms（231 items）；idle 复用帧 ~19ms/帧，静止零件不重栅格。
- 双立绘（两个 Player + 两个 SLA）：首帧 draw 131ms；idle 动画 ~42ms/帧（每 Player ~10–15ms，changed≈43–71/帧）。240 帧无异常、无层节点驱逐。
- 探针只加载单一 PSB，跨 PSB 引用的零件（如 `motion/face_parts/鼻`）解析失败被跳过，因此是**部分渲染**；真实完整立绘的每帧成本只会更高。
- 剩余性能开销的最大单项是移动零件逐帧重栅格；零件栅格走 `tTVPBaseBitmap::InternalAffineBlt`（16.16 定点行级多线程 CPU 循环），`checkQuadSquared` 的 warpAffine 快路径只覆盖 `OperateTriangles` 双三角调用方（转场类），不覆盖 E-mote 零件。PSB 解析本身 <100ms（见 §4.6），不是黑屏来源。
- accurate-SLA 每帧对 internal render layer 的整画布 piledCopy 是无用开销（该层以 `visible=false` 创建且 SLA 路径从不读回），已改为层不可见时跳过；双立绘首帧 131ms → 106ms，稳态帧变化在噪声内。
- 1920×1080 双立绘探针（游戏坐标系校准后）：首帧 **572ms**；稳态每 Player **avgMs≈35–47ms**。真实游戏采样（1280×720，用户统一日志）：draw avgMs≈7.8–9.9/立绘、progress≈2ms/立绘、合成≈3–6ms@1080p，游戏循环整体 ~26fps，即「动作一卡一卡」。
- 卡顿最大单项已修复：失败/已缓存项的原始资源解析（`loadRawSourceVariant`：候选路径 + 存储查找 + 模块加载）曾每帧对 ~130–180 个跨 PSB 引用重跑；`loadRenderSourceByName`/`loadRenderSourceTextureByName` 现复用缓存条目的 `rawSource`/`resolvedKey`，已知失败项走 `backingLoadAttempted` 快速返回。探针实测稳态 **88.5ms → 37.0ms/帧（2.4×）**，每立绘 draw **35–47ms → 8.3–9.6ms（~4.5×）**。
- 采样仪器已配齐：`sla.accurate.first` / `sla.accurate.stats` / `sla.accurate.progress.stats` / `sla.accurate.composite.ms`；若修复后游戏内仍不足 60fps，剩余预算在 draw 外（脚本/背景/文字/呈现），再评估单画布渲染或呈现端优化。
- 2026-08-16 切换语义探针（同一 fixture PSB，game-style switch + `playTimeline("挨拶", 1)`）：
  - 修复前：`play(base.motion, Force)` 启动全部 16 条 main timeline，同一毫秒内 150+ 次控制帧写入把 `head_UD/body_UD/face_*` 拖入 `sample_全自動_test` 等模板轨道的编舞序列（身体晃→头晃→抽搐），与用户「每次切换固定做一套诡异动作、win 原版没有」完全吻合。
  - 修复后：play 后控制帧跨越 0 次；`animating` 空闲为 0；`playTimeline` 后为 1；pose timeline 播完（挨拶 86 帧）回 0 —— 与原版 onSync 边沿触发流程一致。reset seek 应用当前关键帧值（to=0 transition=24/28）而非首个未来帧（to=30 transition=54）。
  - 游戏脚本语义复核：`MotionAffineSourceLayer.tjs` 只经 `playTimeline/stopTimeline/_playTimeline` 驱动姿势 timeline（`emoteMainTimeline` 表由 `getMainTimelineLabelList()` 构建）；`play(base.motion, Force)` 仅装载 clip。M2 公开 manual 的样例流程同为 `play(base.motion)` + 裸 progress/draw。

当前仍缺少：

- 能验证 physics/wind 最终改变节点或像素输出的测试；
- 能验证多参数 Timeline blend 契约的 TJS 成员调用；
- 覆盖 accurate-SLA 渲染路径（tint 同源多色、z 顺序变化帧）的更细自动化回归；
- 带 runtime commit、游戏版本、场景和截图/trace 的 NEKOPARA 回归矩阵，尤其是进游戏黑屏时长与动作期间帧开销的采样（已埋 `sla.accurate.slow` / `SourceCache decode slow` / `ResourceManager::load slow` 采样日志）。

## 7. 下一轮研究顺序

1. 先建立当前 runtime 的行为基线：真实 `Plugins.link`、成员调用、确定性素材和最终像素/节点结果。
2. 按 2016 DLL 已恢复的字段和 idle/close/hold/open 主循环建立可注入 RNG/clock 的 `eyeControl` 状态测试；继续补齐手动状态 1–9 与 edge/node 语义。
3. 按已恢复的三路 outer force 幂插值、128 槽 wind/区间查询、Bust/Pend 积分约束与一帧 staging 定义实现边界；先决定原版未初始化字段的确定性兼容策略，不直接移植 `sdl3-ref/EmotePhysics.cpp`。
4. 对 Timeline/fade/wind/physics API 做参数个数、单位、默认值和错误语义审计。
5. 最后用 NEKOPARA 的明确版本与场景做重度兼容验证，并把“通过了哪条路径”与“仍未证明什么”同时记录。

## 8. 维护规则

- 新结论必须附当前源码位置、可重复命令或具名样本观察；无法验证的写成假设。
- 当前能力只在本文件更新。专题文档保留历史推理，但不得宣称当前完成度。
- 路径、测试或参考实现消失时，先降级结论再调查，不保留“曾经通过所以现在也通过”。
- 原版闭源边界始终保留；除非获得可验证且许可清晰的新材料，不使用“官方源码”“完整复刻”或“行为等价”。
