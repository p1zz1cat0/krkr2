# 原生依赖策略（vcpkg 与 krkr-sys）

[← 索引](README.md)

Rust 层通过 `krkr-sys-*` 链 vcpkg，避免与 C++ 各引一套 zlib/lz4。

---

## 1. 原则：一份 vcpkg，一份链接

| 依赖 | C++ | Rust |
|------|-----|------|
| zlib | `ZLIB::ZLIB` | `krkr-sys-zlib` |
| lz4 | `lz4::lz4` | `krkr-sys-lz4` |
| RL、Emote LZ4 流等 | 逐步删除 | **纯 Rust** |

**禁止：** 同一 so 内 C++ zlib + Rust `flate2/rust_backend` 双份实现。

---

## 2. krkr-sys-* 实现

```cmake
corrosion_set_env_vars(krkr_psb
  KRKR_VCPKG_INSTALLED=${_VCPKG_INSTALLED_DIR}
)
```

```toml
[features]
default = ["vcpkg-zlib"]
standalone = ["dep:flate2"]   # 仅独立 CLI
```

---

## 3. 决策表

| 情况 | 选择 |
|------|------|
| vcpkg 已有且 C++ 仍在用 | `krkr-sys-*` |
| 格式私有 | `rust/crates/...` 纯 Rust |
| 独立 CLI | `standalone` feature |

PSB 模块细节见 [modules/psbfile.md](modules/psbfile.md)。
