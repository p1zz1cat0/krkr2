// storage_fstat.cpp — StoragesFstat 兼容说明。
//
// PackinOne.dll 的 StoragesFstat 是独立 TJS 类（官方逆向文档
// M12-04-01：getFileSize/isExistent/isDirectory/isReadOnly/rename/
// remove/getCreationTime/setCreationTime/getLastAccessTime/copy +
// currentDirectory）。
//
// KrKr2 本体 fstat 插件已注册同名 native 类 "StoragesFstat"（附加到
// Storages 类，提供 fstat/getTime/setTime/copyFile 等官方接口），TJS
// 全局命名空间不允许重复注册同类名。因此 PackinOne 的独立类版本
// 无法与本体共存，文件功能由本体 fstat 提供（方法名不同但能力覆盖）。
//
// 本文件保留占位：确保 packinone 模块加载时 fstat 已注册。若未来遇到
// 脚本硬性调用 PackinOne 风格方法名（如 StoragesFstat.getFileSize），
// 应在 fstat 插件的 StoragesFstat 类上补充方法别名，而不是在本模块
// 重复注册类。
#include "packinone.h"

#define NCB_MODULE_NAME TJS_W("packinone.dll")

static void InitPlugin_PackinOneStorageFstat() {
    ncbAutoRegister::LoadModule(TJS_W("fstat.dll"));
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_PackinOneStorageFstat);
