// Two TJS engines on two threads with the debug registries on.
//
// xp3filter.tjs runs in a separate engine on each thread that reads an
// archive (for example the sound decode thread), at the same time as the
// main engine. The object hash map and the stack tracer are process-wide,
// so they must stay correct when both engines run.

#include <thread>
#include <catch2/catch_test_macros.hpp>

#include "tjs.h"
#include "tjsDebug.h"

namespace {

    // Each call pushes a stack record and each dictionary adds and removes
    // an object hash map record.
    const tjs_char *Script = TJS_W(
        "function make(i) { return %[ \"i\" => i ]; }"
        "var sum = 0;"
        "for(var i = 0; i < 20000; i++) sum += make(i).i;");

    void RunScript() {
        auto *engine = new tTJS();
        engine->ExecScript(Script);
        engine->Shutdown();
        engine->Release();
    }

} // namespace

TEST_CASE("debug registries survive two engines on two threads") {
    TJSAddRefObjectHashMap();
    TJSAddRefStackTracer();
    REQUIRE(TJSObjectHashMapEnabled());

    for(int round = 0; round < 4; round++) {
        std::thread worker(RunScript);
        RunScript();
        worker.join();
    }

    // Each thread has its own call stack, so the main thread sees none of
    // the frames that the worker pushed.
    CHECK(TJSGetStackTraceString(0).IsEmpty());

    TJSReleaseStackTracer();
    TJSReleaseObjectHashMap();
}
