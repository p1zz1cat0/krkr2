// KAGParserExb.dll compatibility alias (Yoghourt)
//
// KAGParserExb and ExtKAGParser both replace the global KAGParser class.
// ExtKAGParser already owns that class and implements the KAGParserExb
// surface (multi-line tags, parameter macros) plus AttribNameMacros.
// This module only makes Plugins.link("KAGParserExb.dll") succeed.
//
// Do not replace the KAGParser class here: loading both modules would
// overwrite ExtKAGParser and lose numeric macros, taglist and local
// variables.

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("KAGParserExb.dll")

extern "C" void TVPKAGParserExbPluginAnchor() {}

// ExtKAGParser installs its KAGParser class when that module is linked.
// KAGParserExb games link only this DLL, so install it here as well.
// extkagparser_init() is idempotent.
void extkagparser_init();

static void KAGParserExbPreRegist() {
    extkagparser_init();
    if(auto logger = spdlog::get("plugin"))
        logger->info("[KAGParserExb] linked; ExtKAGParser provides the "
                     "KAGParser class");
}
NCB_PRE_REGIST_CALLBACK(KAGParserExbPreRegist);
