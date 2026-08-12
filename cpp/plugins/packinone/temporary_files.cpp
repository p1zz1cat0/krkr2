// temporary_files.cpp — TemporaryFiles dependency loader.
//
// PackinOne.dll 的 TemporaryFiles 是独立 TJS 类（add/remove/clear，
// 析构删除登记文件，官方逆向文档 M12-04-01）。
//
// KrKr2's fstat plugin owns the global TemporaryFiles class.  Its class now
// exposes both the original entry/entryFolder API and PackinOne's
// add/remove/clear aliases with real registered-path cleanup.  Loading fstat
// before PackinOne class registration avoids a duplicate global class.
#include "packinone.h"

#define NCB_MODULE_NAME TJS_W("packinone.dll")

extern "C" void TVPPackinOneTemporaryFilesAnchor() {}

static void InitPlugin_PackinOneTemporaryFiles() {
    ncbAutoRegister::LoadModule(TJS_W("fstat.dll"));
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_PackinOneTemporaryFiles);
