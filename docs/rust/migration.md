# 迁移流程

[← 索引](README.md)

---

## 1. 单模块生命周期

```text
① 新建 rust/crates/krkr-<m> + bridge 头 + docs/rust/modules/<m>.md
② C++ 包装委托 Rust（KRKR_USE_RUST_<M>）
③ 字节级 / 单测对齐
④ 删除 C++ 实现，CMake 去掉重复 find_package
⑤ bridge/manifest.toml 标记 stable
```

---

## 2. 目录方案对比

| 方案 | 评价 |
|------|------|
| `cpp/plugins/*/rust/` | ❌ 难扩展 |
| 根目录单 crate | △ 无分层 |
| **`rust/` + `bridge/` + `cpp/`** | ✅ 与 `cpp/` 对称，语言一目了然 |

---

## 3. 模块文档

| 模块 | 文档 |
|------|------|
| psbfile | [modules/psbfile.md](modules/psbfile.md) |
