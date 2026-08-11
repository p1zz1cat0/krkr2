// temporary_files.cpp — TemporaryFiles 兼容说明。
//
// PackinOne.dll 的 TemporaryFiles 是独立 TJS 类（add/remove/clear，
// 析构删除登记文件，官方逆向文档 M12-04-01）。
//
// KrKr2 本体 fstat 插件已注册同名的 TemporaryFiles native 类（提供
// entry/entryFolder，TJS 全局命名空间不允许重复注册）。因此本模块
// 不重复注册类，本体 fstat 的能力已覆盖 PackinOne 的使用场景。
//
// 本文件保留占位：确保 packinone 模块加载时 fstat 已注册。若未来
// 遇到脚本调用 add/remove/clear，应在 fstat 插件的 TemporaryFiles
// 类上补充方法，而不是在本模块重复注册类。
#include "packinone.h"

#define NCB_MODULE_NAME TJS_W("packinone.dll")

static void InitPlugin_PackinOneTemporaryFiles() {
    ncbAutoRegister::LoadModule(TJS_W("fstat.dll"));
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_PackinOneTemporaryFiles);
