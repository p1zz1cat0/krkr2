# KrKr2 插件目录约定

这里的源码按“一个脚本插件一个目录”组织。目录名是维护用的源码边界，
不改变脚本侧的 `NCB_MODULE_NAME`、`Plugins.link("*.dll")` 或任何 TJS API。

## 目录层次

- `common/`：多个插件共享的原生兼容头和 Layer 缓冲辅助实现。
- 一个文件即可表达的插件：放在同名目录中，例如
  `getSample/getSample.cpp`、`Extrans/Extrans.cpp`。
- 多文件插件：在自己的目录中维护实现和 `CMakeLists.txt`，例如
  `psbfile/`、`motionplayer/`、`packinone/`、`extNagano/`。
- `KAGParser/`、`layerExDraw/` 等已有目录保持原有内部结构。

简单插件不单独创建 CMake target，仍由本目录的聚合 `krkr2plugin` 静态库
统一列出源文件；这样不会为了整理目录引入一堆无意义的 target。聚合源使用
`PRIVATE`，子插件只链接 `krkr2plugin`，避免同一个 ncbind 注册单元被重复编译。

## 添加新插件

1. 创建 `cpp/plugins/<PluginName>/`，把实现、头文件和最小维护文档放进去。
2. 在 `cpp/plugins/CMakeLists.txt` 的聚合源列表中加入路径；只有确实需要独立
   依赖、测试或平台分支时才创建子目录 `CMakeLists.txt` 和独立 target。
3. 保留原始模块名和注册锚点。目录名、文件名可以整理，脚本 ABI 不可以顺手改名。
4. 更新对应 smoke fixture 和文档中的源码路径，然后重新配置、编译、CTest 和烟测。

共享头应进入 `common/`，插件私有头应留在插件目录；不要通过复制一份头文件来
绕过 include 路径，也不要把注册源改成 `PUBLIC` 以“修复”静态库保留问题。
