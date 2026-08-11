// packinone.h — PackinOne.dll 兼容插件的公共声明。
//
// 本插件是 Windows PackinOne.dll 的 macOS 原生替代：注册同名模块满足
// Plugins.link("PackinOne.dll") 特征门，并按官方逆向文档（krkrz
// SamplePlugin 教程 M12-04）提供 TJS 可用的接口子集。Windows 专属功能
// （注册表、GDI 字体、对话框）不做。
//
// 源文件拆分：
//   packinone_register.cpp — ncbind 注册入口、System/Layer/Plugins 扩展、
//                            AffineSourceMovie 脚本补丁
//   storage_fstat.cpp      — StoragesFstat TJS 类
//   temporary_files.cpp    — TemporaryFiles TJS 类
//   lz4_stream.cpp/.h      — LZ4 流压缩/解压（手写，无外部依赖）
//   scripts_add.cpp        — ScriptsAdd TJS 类
#pragma once

#include "ncbind.hpp"
#include "ScriptMgnIntf.h"
#include "CharacterSet.h"

#include <filesystem>
#include <string>

// 把 KrKr2 storage 名（可能带 xp3:> 前缀）转成本地文件路径。
std::filesystem::path PackinOneLocalPath(const ttstr &storageName);

// UTF-8 字符串 -> ttstr（KrKr2 的 tjs_char 是 UTF-16）。
ttstr PackinOneFromUtf8(const std::string &s);
