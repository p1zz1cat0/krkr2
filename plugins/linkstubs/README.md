# linkstubs

DLL 模块名空壳。本构建不携带这些 DLL 文件，只登记名字，让旧 KAG 脚本的
`Plugins.link("x.dll")` 探测成功。

一个名字一个 `.cpp`。文件之间不共享状态，删掉一个只影响它自己。

## 能加的名字

核心已经提供同等能力，并且脚本侧不需要这个 DLL 的 TJS 类。每个文件头写了
名字与核心实现的对应。

## 不能加的名字

核心没有同名 TJS 类时不要加。link 成功会骗过脚本的探测分支，脚本随后调用
缺失的成员，抛 TJS 异常。已知属于这类的是 `dmmcloud.dll`、`krkrsteam.dll`、
`win32ole.dll`、`k2compat.dll`（`WindowTouchMouse`、`ModelessOwnerWindow`）、
`kztouch.dll`（`KzTouchWindow`、`TouchHandler`）。

## 接线

加一个名字要同步 7 处。步骤和模板见 `guides/0017-krkr2-compat.md`。

验证：

```sh
scripts/run-plugin-quality-gate.sh --plugin <name>
```
