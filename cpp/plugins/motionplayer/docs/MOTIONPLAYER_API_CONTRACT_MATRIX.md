# MotionPlayer / EmotePlayer API 契约矩阵

> **接口证据表，不是兼容性证明。** 本表对照 M2 公开 2020 KiriKiri Sample、NEKOPARA 所带 2016 Windows DLL 的静态行为，以及当前 KrKr2 注册与实现。方法名存在只证明脚本入口存在；只有参数、状态变化、最终节点/像素结果和生命周期都经过测试，才能称为行为兼容。
>
> 当前能力与施工顺序仍以 [研究基线](MOTIONPLAYER_RESEARCH.md) 为唯一权威入口。
>
> 最后核对：2026-08-14

## 1. 对照对象与证据边界

| 对照对象 | 身份 | 本表采用的证据 | 不能据此推出 |
|---|---|---|---|
| M2 KiriKiri Sample | 2020-06-08 `emoteplayer.dll`；下载包 SHA-256 `7b5002ad122d24c40a2b1ab086e34c1d4ee0bcef127b98cf1e1ab53089514c6e` | `manual.tjs`、`emoteplayer.ks`、DLL 元数据 | NEKOPARA 2016 版本的内部行为 |
| NEKOPARA Windows 插件 | 2016-02-05 `emoteplayer.dll`，SHA-256 `c29474887d7ef2533f3ff19e24f63a607a2e246373b9d0cf394f5d57a7a633c5` | NCB/RTTI、静态调用图、控制对象字段/状态、已闭合 helper 数学与游戏侧素材 | 动态最终画面、样本未定义字段的设计意图或跨版本等价 |
| FreeMote-SDK | v3.82，commit `4340e8fc2138f013157d5b9cd40ba17d01347093` | `iemote.h` 接口、WebGL wrapper 调用 | 与 M2 插件等价；wrapper 自身无 bug |
| 当前 KrKr2 | 2026-08-14 工作树 | `main.cpp` NCB 注册、`EmotePlayer`/`Player` 实现及真实消费者 | 尚未执行的商业游戏兼容性 |

状态含义：

- **实现**：当前存在与公开契约形状相符的注册和执行路径；仍需行为测试。
- **部分**：入口存在，但参数、默认值、单位、返回值或消费者不完整。
- **无效果**：入口可调用，但当前没有可证明的最终节点或像素影响。
- **待验证**：静态代码不足以确认与参考版本一致。

2016 DLL 的 physics/blink/wind/outer-force 行已完成比名称扫描更深的静态分析，具体证据见[闭源行为研究](MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md)。其它只写“名称线索”的行仍不表示已恢复签名。公开 2020 手册与 FreeMote 交叉一致时，本表仍把它写成“跨版本候选契约”，而不是直接归属于 2016 DLL。

## 2. 生命周期、资源与帧推进

| API / 契约 | M2 2020 公开样例 | NEKOPARA 2016 DLL | 当前 KrKr2 | 状态与主要风险 | 需要的验收证据 |
|---|---|---|---|---|---|
| `Motion.ResourceManager(window, cacheSize)`；`load` / `unload` / `unloadAll` | 手册声明；样例用 `load()` 返回资源与 metadata | 类名和方法名线索 | 已注册并有缓存/加载路径 | **待验证**：错误、缓存上限和卸载后引用语义未与原版对照 | 真实 `Plugins.link` 后加载 PSB/MTN；缺失、重复加载、卸载与再加载 |
| `Motion.EmotePlayer(resourceManager)`；设置 `motionKey` / `chara` 后播放 | 手册与 `emoteplayer.ks` 有完整调用链 | 类名、属性和播放入口线索 | 已注册，内部委托 `Player` | **部分**：基本生命周期存在，跨版本失败语义未知 | 同一资源的创建、切换、销毁和双 Player 前后页生命周期 |
| `progress(diffTick)`，TJS 单位为 ms | 明确为毫秒；底层 driver 换算为 1/60 frame | 确认 `ms * 60 / 1000` 后进入 `frameProgress` | 接受可选数值，调用 `progressMsLike_0x6D2A54` | **实现/待验证**：负值或大值被归零；与原版边界语义未对照 | 16/33/1000 ms 的确定性节点/Timeline 推进；负值、缺参和长帧 |
| `draw(layerOrAdaptor)` | 直接 Layer 或 `SeparateLayerAdaptor`；样例推荐 adaptor | `draw` 与 adaptor 名称线索 | 两类入口均注册 | **待验证**：存在绘制路径不等于像素、mask 和 blend 正确 | 最终 surface/pixel 断言；Layer 与 adaptor 输出对照；自然 present 生命周期 |
| `skip()` / `pass()` | 手册声明 | 名称线索 | 已注册并有实现 | **待验证**：目标时点与 callback/Timeline 关系未知 | 确定性动画的跳过终点、经时推进和回调次数 |
| `serialize()` / `unserialize(data)` | 样例用于前后 Player 状态交接 | 名称线索 | 保存基础变量、时间和 Timeline 信息 | **部分**：physics、wind、outer force、插值器等状态未证明完整 | round-trip 后节点、变量、Timeline 与可见输出一致；畸形数据失败语义 |
| `initPhysics(data.metadata)` | 每次加载/切换素材后调用 | 确认解析 bust/hair/parts/eye 等控制表并创建持久对象 | 已事务化解析、自有化、绑定 baseLayer，并按 motion generation 提交 | **内核/管线已实现，动态验收未闭合**：失败保留同素材旧状态，切换素材后旧 generation 休眠 | metadata 驱动的变量/节点与最终像素随帧变化；切换/失败生命周期 |

## 3. 变换、变量与物理控制

| API / 契约 | M2 2020 公开样例 | NEKOPARA 2016 DLL | 当前 KrKr2 | 状态与主要风险 | 需要的验收证据 |
|---|---|---|---|---|---|
| `setCoord(x,y,time=0,easing=0)` | 参数与 ms/easing 语义明确 | 名称线索 | 兼容回调接受默认参数并驱动 animator | **实现/待验证** | 立即值、过渡中间值、正负 easing 与完成值 |
| `setScale(scale,time=0,easing=0)` | 参数与 ms/easing 语义明确 | 名称线索 | 兼容回调及 animator 存在 | **实现/待验证** | 同上，并覆盖负值/零值和 transform order |
| `setRotate(rad,time=0,easing=0)` | 角度单位为 rad | 名称线索 | `setRotate`/`setRot` 共用兼容回调 | **实现/待验证**：单位必须由输出验证 | π/2 的节点矩阵与像素方向；过渡/easing |
| `setColor(0xAARRGGBB,time=0,easing=0)` | 参数与颜色格式明确 | 名称线索 | 兼容回调及颜色 animator 存在 | **实现/待验证** | ARGB 通道、插值和 blend 后最终像素 |
| `setVariable(name,value,time=0,easing=0)` / `getVariable` | 完整四参 setter | 名称线索 | 兼容回调与 controller 路径存在 | **部分**：变量可写不等于所有 controller 正确消费 | 具名变量的立即/插值值、未知名称、最终节点和像素 |
| `setDrawAffineTranslateMatrix(a,b,c,d,tx,ty)` | 六参矩阵 | 名称线索 | 六参兼容回调已注册 | **待验证** | 已知矩阵下的节点坐标与像素位置 |
| `setOuterForce(name,x,y,time=0,easing=0)` | 五参契约 | 确认 3–5 参、`bust`/`hair`/`parts` 分流、队列和 `pow(progress,k)` 曲线；`k=e+1` / `1/(1-e)` / `1` | 接收五参，但只路由 `bust` / `h` / `parts` 并保存状态 | **无效果，P0**：原版与公开 wrapper 均使用 `hair`；当前无 physics 消费者 | 三类 label 的立即值/中间值/完成值、正负 easing、节点/像素变化 |
| `startWind(start,goal,speed,powMin,powMax)` / `stopWind()` | 五参含义明确 | 确认 128 槽生命周期、停止销毁、`abs(x-position)<=2*power` 第一命中查询与 `power*signedSpeed` 返回 | 接受五值，但内部仍命名为角度/振幅/频率且只存状态 | **无效果，P1**：状态模型错误且无消费者 | 固定随机种子/时钟下的槽生成、重叠第一命中、停止及最终节点变化 |
| `hairScale` / `bustScale` / `partsScale` | 三个物理幅度属性 | 确认三个字段分别传入 hair/bust/parts 每帧消费者 | 属性注册并写入 `Player` | **部分**：没有 physics 消费者时只是状态 | scale=0/1/2 对相同物理输入的振幅与像素对照 |
| `eyeControl` metadata / 自动眨眼 | 样例资产与 driver 类型可提供控制线索；手册无独立 blink API | 确认字段、13 状态对象和 idle→close→hold→open 主循环，并写回具名值 | 只收集 control bindings，未发现自动眨眼状态机 | **无效果，P0** | 固定 RNG/时钟下睁眼→闭眼→睁眼状态和最终眼部节点/像素 |

## 4. Timeline 契约

| API / 契约 | M2 2020 公开样例 | NEKOPARA 2016 DLL | 当前 KrKr2 | 状态与主要风险 | 需要的验收证据 |
|---|---|---|---|---|---|
| `playTimeline(name,flags=0)` / `stopTimeline(name="")` | 默认值与主/差分 flags 明确 | 名称与 flag 线索 | 已注册并维护活动 Timeline | **实现/待验证**：空 label、重复播放与 flags 合并语义待核 | 主/差分/并行组合、重复播放、空 label 停止和自然结束 |
| `getTimelinePlaying(name="")` / `getLoopTimeline(name)` | 签名与返回类型明确 | 名称线索 | 已注册 | **待验证** | 播放前、中、结束后及未知 label 返回值 |
| `getTimelineTotalFrameCount(name)` | 返回总帧数 | 名称线索 | 已注册 | **待验证** | 与 PSB timeline 数据逐项核对；未知 label 失败语义 |
| main/diff label list | `getMainTimelineLabelList()` / `getDiffTimelineLabelList()` | 名称线索 | 已注册 | **待验证** | 顺序、重复项、空列表和 PSB 数据一致性 |
| `setTimelineBlendRatio(name,ratio,time=0,easing=0,stopWhenBlendDone=false)` | 五参契约明确 | 名称线索 | 当前只接收 `name, ratio`；ratio 有消费者 | **部分，P1**：丢失过渡、easing 和完成后停止 | 五参数真实成员调用；中间 blend、easing、完成停止和最终像素 |
| `fadeInTimeline(name,time=0,easing=0)` | 第三参是 easing | 名称线索 | 当前第三参声明/处理为整数 flags | **部分，P1**：参数语义错误，零时长还把第三参传给 play flags | 三参调用的中间 blend、正负 easing、零时长行为 |
| `fadeOutTimeline(name,time=0,easing=0)` | 第三参是 easing | 名称线索 | 当前第三参声明/处理为整数 flags | **部分，P1**：同上 | 三参调用、结束停止时点与信息列表一致性 |
| `getPlayingTimelineInfoList()` | 元素含 `label`、`flags`、`blendRatio`；返回 flags 含 Parallel | 名称线索 | 已注册并生成字典列表 | **待验证** | 字段类型、顺序、flags 规范化和 blend 中间值 |

## 5. 属性与辅助入口

| API / 契约 | M2 2020 公开样例 | NEKOPARA 2016 DLL | 当前 KrKr2 | 状态与主要风险 | 需要的验收证据 |
|---|---|---|---|---|---|
| `chara` / `motion` / `motionKey` / `completionType` / `maskMode` | 手册声明 | 属性名线索 | 已注册 | **部分/待验证**：属性可读写不证明切换、副作用或枚举值一致 | 设置顺序、资源切换、无效值、mask 最终像素 |
| `animating` | 只读动画状态 | 名称线索 | 只读属性已注册 | **待验证** | 变换、变量、Timeline、physics 各自开始/结束时状态 |
| `meshDivisionRatio` | 可读写属性 | 名称线索 | 已注册 | **待验证** | 网格密度、边界值与最终几何输出 |
| `useD3D` / `Motion.getD3DAvailable()` | 公开样例推荐可选 D3D | D3D 类/入口线索 | 属性和查询已注册；非 Windows 后端不是原 D3D | **部分**：应验证兼容失败语义，不宣称 D3D 等价 | 当前平台返回值、切换前后渲染路径与无崩溃/无静默错误 |
| `SeparateLayerAdaptor(targetLayer)`；`targetLayer` / `absolute` / `clear` / `assign` | 手册声明并推荐用于 draw | 类与成员名线索 | 类和主要成员已注册 | **待验证** | 目标 layer 绑定、clear/assign、absolute 及 draw 合成结果 |

## 6. 当前实施优先级

| 优先级 | 契约缺口 | 原因 | 完成条件 |
|---|---|---|---|
| P0 | `initPhysics`、`eyeControl` 自动眨眼、outer-force 消费链 | 当前入口存在但没有可证明的最终效果，直接对应重度 E-mote 角色的核心表现 | 确定性单测 + 真实 `Plugins.link` 调用 + 节点/像素结果 |
| P1 | Timeline blend 五参、fade easing、wind 参数与消费者 | 已发现明确的公开签名不一致或状态止于存储 | 参数/默认值/错误语义测试；过渡中间值与最终结果 |
| P2 | 查询、序列化、adaptor、D3D 兼容语义及边界 | 表面 API 覆盖较广，但返回值和异常路径尚未逐项对照 | 完整成员调用矩阵与失败/清理测试 |

## 7. 最小兼容验收矩阵

每个修复项至少同时通过以下四层，才可以在研究基线中从“部分/无效果”升级：

1. **注册层**：用真实 `Plugins.link("motionplayer.dll")` 与 `Plugins.link("emoteplayer.dll")` 获取类、构造实例并调用成员；不以符号 anchor 代替。
2. **契约层**：覆盖完整参数、缺省参数、错误参数、单位、返回类型和未知 label；TJS 调用不能只走 C++ 直调。
3. **状态层**：固定时钟和随机种子，断言过渡中间值、完成值、停止/回调时点以及 serialize round-trip。
4. **结果层**：验证节点矩阵或最终 surface/pixel；窗口出现、进程存活、固定延时和无崩溃都不算功能成功。

完成以上层级仍只证明对应 fixture 与调用路径。NEKOPARA 兼容性必须另外记录游戏版本、素材族（基础 / `dx_` / `dxlow_`）、场景、runtime commit 和可见结果。

## 8. 参考

- [M2 E-mote SDK 下载页](https://emote.mtwo.co.jp/download/sdk/)
- [FreeMote-SDK README（固定 commit）](https://github.com/Project-AZUSA/FreeMote-SDK/blob/4340e8fc2138f013157d5b9cd40ba17d01347093/README.md)
- [FreeMote `IEmotePlayer` 接口（固定 commit）](https://github.com/Project-AZUSA/FreeMote-SDK/blob/4340e8fc2138f013157d5b9cd40ba17d01347093/Windows-DX9/iemote.h)
- [FreeMote WebGL wrapper（固定 commit）](https://github.com/Project-AZUSA/FreeMote-SDK/blob/4340e8fc2138f013157d5b9cd40ba17d01347093/WebGL/driver/emoteplayer.js)
- [`manual.tjs`](../manual.tjs)
- [研究基线](MOTIONPLAYER_RESEARCH.md)
- [2016 闭源行为研究](MOTIONPLAYER_CLOSED_SOURCE_BEHAVIOR_RESEARCH.md)
