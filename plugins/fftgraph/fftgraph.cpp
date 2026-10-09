#include "ncbind.hpp"

#define NCB_MODULE_NAME TJS_W("fftgraph.dll")

extern "C" void TVPFftGraphPluginAnchor() {}

static void InitPlugin() {
    TVPExecuteScript(TJS_W("function drawFFTGraph(){}"));
}

NCB_PRE_REGIST_CALLBACK(InitPlugin);
