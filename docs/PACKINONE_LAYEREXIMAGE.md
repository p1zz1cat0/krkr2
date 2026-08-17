# PackinOne `layerExImage` 兼容面

## 证据边界

实现依据是用户提供的 `PackinOne.dll` fixture（PE32 / x86，SHA-256：
`5a818e93dc2b653cdcc133639a26f996eb704bc896c606e1a31b4cae315e7047`）。
fixture 的 RTTI / ncbind 符号确认了 `layerExImage` 类及以下六个成员的
调用形状：

| 成员 | 参数形状 |
| --- | --- |
| `light` | `(int, int)` |
| `colorize` | `(int, int, number)` |
| `modulate` | `(int, int, int)` |
| `noise` | `(int)` |
| `generateWhiteNoise` | `()` |
| `gaussianBlur` | `(float)` |

本地实现通过 `Plugins.link("PackinOne.dll")` 把成员挂到 `Layer`，保持
PackinOne 的兼容入口。fixture 没有为 `resize`、`quality`、`loadCxImage`、
`saveCxImage` 或 `flip` 提供足够证据，因此没有注册这些接口，也没有猜测
`quality = 0/1/2` 的含义。

像素算法与官方参考实现对照：
<https://github.com/krkrz/krkr2/tree/master/kirikiri2/trunk/kirikiri2/src/plugins/win32/layerExImage>

`generateWhiteNoise` 保留随机性质：只保证 RGB 三通道相等、alpha/stride
padding 不变，不承诺固定随机值。`gaussianBlur` 在进入卷积前拒绝
NaN、无穷、绝对值大于 64 的输入；有限负数按参考实现的 `fabs` 语义处理。
64 是 macOS 端资源安全上限，不是 Windows ABI 或质量等级映射。

实现使用 CxImage 7.0.2 中等价的像素处理/高斯卷积逻辑，但没有引入旧的
Windows codec 栈。CxImage 的版权和三条再分发条件见同目录的
`CXIMAGE_LICENSE.txt`；这份实现是 macOS 端的改写，不是原始 Windows
代码的未修改副本。

## DLL 注册提取记录（fixture 专属）

2026-08-12 使用 `radare2` 对上述 SHA-256 fixture 做了 PE/RTTI/注册函数
交叉检查。地址是该 fixture 的 PE image VA，只用于复核这一个文件，不能
当作可迁移 ABI：

- 文件类型：PE32 / i386；导出 `V2Link`（RVA `0x1030`）、`V2Unlink`
  （RVA `0x1080`），同时存在 `_V2Link@4`、`_V2Unlink@0` 别名。
- 模块字符串：`layerExImage.dll`；ncbind attach 类字符串：
  `layerExImage` / `ncbAttachTJS2ClassAutoRegister<layerExImage>`。
- 注册函数 VA `0x100358c0` 依次把以下名字传给注册包装器：
  `light`、`colorize`、`modulate`、`noise`、`generateWhiteNoise`、
  `gaussianBlur`。该函数被初始化链上的两个调用点（VA `0x10034f7e`
  和 `0x1003775b`）引用；它们最终由 `V2Link` 的注册遍历触达。
- 同一 `.data` 区的 RTTI/ncbind 方法签名为：

  | 方法 | MSVC RTTI 方法形状 | 结论 |
  | --- | --- | --- |
  | `light` | `AEXHH@Z` | `(int, int)` |
  | `colorize` | `AEXHHN@Z` | `(int, int, double)` |
  | `modulate` | `AEXHHH@Z` | `(int, int, int)` |
  | `noise` | `AEXH@Z` | `(int)` |
  | `generateWhiteNoise` | `AEXXZ` | `()` |
  | `gaussianBlur` | `AEXM@Z` | `(float)` |

对 DLL 中 ASCII/UTF-16 字符串和注册函数都没有发现 `quality`、`resize`、
`loadCxImage`、`saveCxImage`、`flip` 的注册项。因此这些接口继续保持
“未确认、未实现”，不能仅凭 CxImage 被编进 DLL 就推断它们属于
`layerExImage` 的脚本 ABI。

DLL 还包含明确的 CxImage 7.0.2 版权块（`CxImage Copyright START/END`）
以及 `Image not of any known type, or corrupt`、`Image too large to decode`
等解码错误文本。这证明原 DLL 包含 CxImage 代码/资源，但不改变上面的
`layerExImage` 六方法注册边界。
