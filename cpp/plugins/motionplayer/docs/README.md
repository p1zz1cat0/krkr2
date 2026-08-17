# MotionPlayer 文档索引

> **先读：** [MOTIONPLAYER_RESEARCH.md](MOTIONPLAYER_RESEARCH.md) 是当前现状、证据边界与施工优先级的唯一权威入口。
>
> 原版 `motionplayer.dll` 与 `emoteplayer.dll` 均为闭源商业插件；本目录没有已验证的原版源码或完整行为规范。
>
> 代码路径：`cpp/plugins/motionplayer/`

本目录跨越过多轮架构和调查。除当前研究基线外，其余专题主要用于保存历史样本、排查思路和术语；不能仅凭文档标题、旧日志或旧测试命令判断当前 runtime 已实现或已验证某项能力。

## 阅读顺序

| 文档 | 定位 | 使用方式 |
|---|---|---|
| [MOTIONPLAYER_RESEARCH.md](MOTIONPLAYER_RESEARCH.md) | **当前权威** | 先确认当前能力、缺口、证据等级和已退役结论 |
| [MOTIONPLAYER_API_CONTRACT_MATRIX.md](MOTIONPLAYER_API_CONTRACT_MATRIX.md) | 当前接口证据矩阵 | 对照 M2 2020、NEKOPARA 2016 二进制线索与当前 KrKr2；不作为行为兼容证明 |
| [MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md](MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md) | 2016 闭源行为静态研究 | 查 metadata、blink、wind、outer force、scale 与 per-frame 消费链；不是当前实现状态或动态兼容证明 |
| [MOTIONPLAYER_API_GUIDE.md](MOTIONPLAYER_API_GUIDE.md) | 接口调查 | 用于查名称与脚本形状；最终以当前 NCB 注册、源码和真实调用为准 |
| [MOTIONPLAYER_PSB_STRUCT.md](MOTIONPLAYER_PSB_STRUCT.md) | 数据调查 | 用于查样本字段；最终以当前 parser/consumer 为准 |
| [MOTIONPLAYER_PROGRESS.md](MOTIONPLAYER_PROGRESS.md) | 混合现状/历史勘误 | 用于追踪 progress 研究；不能代替当前源码审计 |
| [MOTIONPLAYER_ARCHITECTURE.md](MOTIONPLAYER_ARCHITECTURE.md) | 历史架构 | 主要描述旧 `emotefile` 管线，不代表当前 `Player` 架构 |
| [MOTIONPLAYER_MATRIX_PIPELINE.md](MOTIONPLAYER_MATRIX_PIPELINE.md) | 历史渲染调查 | 保留坐标与矩阵假设，引用前重新对照当前实现 |
| [MOTIONPLAYER_TVP_COORDINATES.md](MOTIONPLAYER_TVP_COORDINATES.md) | 历史坐标调查 | 保留 TVP/SDL3 对照，不是当前行为保证 |
| [MOTIONPLAYER_DRAW_VISIBILITY.md](MOTIONPLAYER_DRAW_VISIBILITY.md) | 历史故障排查 | 旧日志字段和旧类名可能已失效 |
| [MOTIONPLAYER_TEXTURE_WORLD_COORDS.md](MOTIONPLAYER_TEXTURE_WORLD_COORDS.md) | 历史字段/坐标调查 | 对当前 node/parser 重新核对后再使用 |
| [MOTIONPLAYER_HEAD_FACE_FIX.md](MOTIONPLAYER_HEAD_FACE_FIX.md) | 历史样本修复记录 | 只支持当时素材与流程，不证明普遍兼容 |
| [MOTIONPLAYER_RENDER_TEST.md](MOTIONPLAYER_RENDER_TEST.md) | 退役测试说明 | 所列 render 脚本和 motionplayer 单测当前不存在，不能照抄执行 |

## 参考材料分类

| 材料 | 当前定位 |
|---|---|
| `../manual.tjs` | M2 公开 2020 KiriKiri Sample SDK 手册的规范化/拼写修正版；证明公开样例 API 形状，不证明内部算法 |
| `origin/*.dll`、`origin/*.xp3` | 未跟踪的本地原版/游戏证据；2016 `emoteplayer.dll` 已用于控制状态机静态研究，仍不等于动态行为或跨版本等价；不进入 Git |
| M2 公开 KiriKiri Sample SDK | `manual.tjs`、`emoteplayer.ks`、2020 DLL 与 PSB v3 的具名版本证据 |
| FreeMote-SDK | 第三方公开 `IEmotePlayer` 接口与 Pure PSB 行为 oracle；KiriKiri 未实现，编译后核心不能当源码 |
| `tests/test_files/emote/*.json` | 从素材导出的结构样本，不是 runtime 格式或完整格式规范 |
| `sdl3-ref/` | 与历史提交 `c16210f` 对应的本项目实验快照；不是原版/M2 源码，不是当前 runtime |
| Android `libkrkr2.so` 研究样本 | stripped 二进制的静态线索；不是仓库构建输入，也不能替代行为验证 |
| 游戏脚本、trace 与截图 | 仅在记录游戏版本、场景、runtime commit 和可见结果后作为窄范围证据 |

特别注意：`sdl3-ref/EmotePhysics.cpp` 明确带有 AI/演示性质，不能称为“完整头发物理”或直接移植基线；`EmoteFileCore.cpp` 的眨眼轮廓也不是当前 runtime 已实现自动眨眼的证据。

## 当前代码入口

| 主题 | 优先检查 |
|---|---|
| 插件注册 / TJS API | `main.cpp`、各 NCB 注册与 `Player*.cpp` 方法 |
| PSB/MTN 加载 | `PlayerMotionLoad.cpp`、`RuntimeSupport.cpp` |
| 节点树 / transform order | `NodeTree.cpp`、`PlayerUpdateLayersInternal.h`、`PlayerUpdateLayerEval.cpp` |
| 帧推进 / controller / blend | `PlayerFrameProgress.cpp`、`PlayerTimeline.cpp` |
| physics / wind | `PlayerCore.cpp` 及真实消费者；不要从方法存在推导效果存在 |

## 维护规则

1. 当前能力和缺口只更新 `MOTIONPLAYER_RESEARCH.md`，避免多篇文档产生互相冲突的“现状”。
2. 专题文档必须保留历史/非权威标记；若全面按当前源码重验，才可升级定位。
3. API 结论同时核对 M2 公开样例、当前注册、方法签名和真实 TJS 调用；公开手册仍不能单独证明内部行为。
4. 测试结论必须确认文件仍存在并执行当前命令；marker、窗口出现或旧日志不算功能成功。
5. 不提交游戏原文件、用户路径或来源/许可不清的闭源材料。

| 日期 | 说明 |
|---|---|
| 2026-08-14 | 建立当前研究基线，明确闭源边界，降级旧架构/旧测试/历史参考实现 |
