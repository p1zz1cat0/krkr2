# MotionPlayer 历史渲染测试说明（已退役）

> [!CAUTION]
> **本文已退役，不提供可执行命令，也不能引用为当前测试通过证据。** 当前验证基线见 [MOTIONPLAYER_RESEARCH.md](MOTIONPLAYER_RESEARCH.md#6-当前验证基线)。

旧版本曾记录一套窗口 smoke、离屏 Catch2 测试和调试日志流程。2026-08-14 核对时，下列关键输入均已不存在：

- `tests/test_files/render/run.sh`
- `tests/unit-tests/plugins/motionplayer-dll.cpp`
- `tests/unit-tests/plugins/motionplayer-render.cpp`
- motionplayer 对应的 `tests/unit-tests/plugins/CMakeLists.txt` target
- `tests/plugin-quality-gates.json` 中的 motionplayer gate

因此，旧文档里的“✅”、超时返回、窗口出现、固定帧数和像素统计不得继续传播为当前结论。需要恢复测试时，应从当前 `Player` 架构重新设计，并至少验证：

1. 真实 `Plugins.link` 后的成员注册与调用；
2. 确定性 PSB/MTN fixture 的加载失败与成功语义；
3. `progress` 后节点、controller 或最终像素发生预期变化；
4. 自动眨眼、Timeline blend、physics/wind 分别有可观察的最终结果；
5. 自然生命周期、取消和清理，而不是依赖 kill/timeout 作为成功判据。

历史命令和日志仍可从 Git 历史检索，但恢复前必须逐项验证路径、target、fixture 与断言仍然成立。
