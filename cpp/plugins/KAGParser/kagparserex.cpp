// KagParserEx.dll 已经内置到core，此处仅占位兼容
#include "ncbind.hpp"

#define NCB_MODULE_NAME TJS_W("KAGParserEx.dll")

static void InitPlugin_KAGParserEx() {}

NCB_PRE_REGIST_CALLBACK(InitPlugin_KAGParserEx);