# CMake 与构建集成

[← 索引](README.md) · 选型见 [ffi.md](ffi.md)

---

## 1. 入口

```cmake
include(${CMAKE_SOURCE_DIR}/cmake/KrkrRust.cmake)
krkr_rust_init()

krkr_rust_crate(NAME psb CRATE krkr-psb)   # → Krkr::Rust::psb

target_link_libraries(psbfile PRIVATE Krkr::Rust::psb)
target_include_directories(psbfile PRIVATE ${KRKR_BRIDGE_INCLUDE})
```

---

## 2. 输出目录

```text
out/linux/debug/
  rust-target/x86_64-unknown-linux-gnu/debug/libkrkr_psb.a
  bridge/include/krkr/psb.h
```

`CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-target`（不用仓库根 `target/`）。

---

## 3. KrkrRust.cmake（概念）

```cmake
function(krkr_rust_crate NAME CRATE)
  corrosion_import_crate(MANIFEST_PATH rust/Cargo.toml CRATES ${CRATE})
  corrosion_set_env_vars(${CRATE}
    KRKR_VCPKG_INSTALLED=${KRKR_VCPKG_INSTALLED}
    KRKR_BRIDGE_OUT=${CMAKE_BINARY_DIR}/bridge/include
  )
  add_library(Krkr::Rust::${NAME} ALIAS ${CRATE})
endfunction()
```

```rust
// rust/crates/krkr-psb/build.rs
fn main() {
    krkr_ffi::emit_cbindgen("psb");
    println!("cargo:rerun-if-changed=src/ffi.rs");
}
```
