#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "YoghourtTelemetryEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace yoghourt_telemetry;

namespace {

TelemetryRecord frameRecord() {
    TelemetryRecord record;
    record.kind = RecordKind::frame;
    record.monotonicNs = 123456789;
    record.frame.frameIndex = 7;
    record.frame.engineIntervalMs = 16.666;
    record.frame.hasInterval = true;
    record.frame.gpuFrameMs = 4.25;
    record.frame.presented = true;
    record.frame.telemetryDropped = 2;
    record.frame.criticalTelemetryDropped = 1;
    return record;
}

bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("frame line carries the reserved prefix and envelope", "[telemetry][encoder]") {
    TelemetryEncoder encoder("0ABC-DEF");
    const std::string line = encoder.encode(41, frameRecord());
    REQUIRE(contains(line, "[YOGHOURT_TELEMETRY_V1]"));
    REQUIRE(contains(line, "\"version\":1"));
    REQUIRE(contains(line, "\"sessionID\":\"0ABC-DEF\""));
    REQUIRE(contains(line, "\"sequence\":41"));
    REQUIRE(contains(line, "\"monotonicNs\":123456789"));
    REQUIRE(contains(line, "\"kind\":\"frame\""));
    REQUIRE(contains(line, "\"frameIndex\":7"));
    REQUIRE(contains(line, "\"engineIntervalMs\":16.666"));
    REQUIRE(contains(line, "\"gpuFrameMs\":4.250"));
    REQUIRE(contains(line, "\"presented\":true"));
    REQUIRE(contains(line, "\"telemetryDropped\":2"));
    REQUIRE(contains(line, "\"criticalTelemetryDropped\":1"));
    REQUIRE(line.back() == '\n');
    // Exactly one line.
    REQUIRE(std::count(line.begin(), line.end(), '\n') == 1);
}

TEST_CASE("unavailable numeric fields are omitted instead of NaN", "[telemetry][encoder]") {
    TelemetryRecord record = frameRecord();
    record.frame.hasInterval = false;
    record.frame.gpuFrameMs = -1.0;
    TelemetryEncoder encoder("s");
    const std::string line = encoder.encode(1, record);
    REQUIRE_FALSE(contains(line, "engineIntervalMs"));
    REQUIRE_FALSE(contains(line, "gpuFrameMs"));

    record.frame.gpuFrameMs = std::nan("");
    REQUIRE_FALSE(contains(encoder.encode(2, record), "gpuFrameMs"));
}

TEST_CASE("reason is emitted only when present", "[telemetry][encoder]") {
    TelemetryEncoder encoder("s");
    TelemetryRecord anomaly = frameRecord();
    anomaly.frame.reason = kFrameReasonAnomaly;
    REQUIRE(contains(encoder.encode(1, anomaly), "\"reason\":\"anomaly\""));

    TelemetryRecord burst = frameRecord();
    burst.frame.reason = kFrameReasonBurst;
    REQUIRE(contains(encoder.encode(2, burst), "\"reason\":\"burst\""));

    REQUIRE_FALSE(contains(encoder.encode(3, frameRecord()), "\"reason\""));
}

TEST_CASE("json strings are escaped", "[telemetry][encoder]") {
    TelemetryEncoder encoder("bad\"id\\x");
    TelemetryRecord record;
    record.kind = RecordKind::event;
    record.eventType[0] = 0;
    std::snprintf(record.detail, sizeof(record.detail), "%s", "quote\" slash\\ tab\t");
    const std::string line = encoder.encode(1, record);
    REQUIRE(contains(line, "\"sessionID\":\"bad\\\"id\\\\x\""));
    REQUIRE(contains(line, "quote\\\" slash\\\\ tab\\t"));
}

TEST_CASE("snapshot and aggregate lines carry window stats and drop counters", "[telemetry][encoder]") {
    TelemetryRecord record;
    record.kind = RecordKind::aggregate;
    record.monotonicNs = 99;
    record.stats.fps = 59.94;
    record.stats.intervalCount = 120;
    record.stats.intervalP50Ms = 16.7;
    record.stats.intervalP99Ms = 33.1;
    record.stats.intervalMaxMs = 50.2;
    record.stats.gpuCount = 118;
    record.stats.gpuP50Ms = 4.0;
    record.stats.gpuP99Ms = 9.5;
    record.stats.gpuMaxMs = 12.0;
    record.stats.lastGpuFrameMs = 5.0;
    record.stats.gpuTimingLateDrop = 0;
    record.stats.telemetryDropped = 3;
    record.stats.criticalTelemetryDropped = 4;
    TelemetryEncoder encoder("s");
    const std::string line = encoder.encode(10, record);
    REQUIRE(contains(line, "\"kind\":\"aggregate\""));
    REQUIRE(contains(line, "\"fps\":59.94"));
    REQUIRE(contains(line, "\"intervalCount\":120"));
    REQUIRE(contains(line, "\"gpuTimingLateDrop\":0"));
    REQUIRE(contains(line, "\"telemetryDropped\":3"));
    REQUIRE(contains(line, "\"criticalTelemetryDropped\":4"));
}

TEST_CASE("burst and lifecycle lines carry range fields", "[telemetry][encoder]") {
    TelemetryRecord burst;
    burst.kind = RecordKind::burst;
    burst.monotonicNs = 5;
    std::snprintf(burst.eventType, sizeof(burst.eventType), "%s", "frame_drop_burst");
    burst.startFrameIndex = 100;
    burst.endFrameIndex = 699;
    burst.triggerFrameIndex = 400;
    burst.targetFrameCount = 600;
    burst.aux = 2;
    TelemetryEncoder encoder("s");
    const std::string line = encoder.encode(1, burst);
    REQUIRE(contains(line, "\"kind\":\"burst\""));
    REQUIRE(contains(line, "\"startFrameIndex\":100"));
    REQUIRE(contains(line, "\"endFrameIndex\":699"));
    REQUIRE(contains(line, "\"triggerFrameIndex\":400"));
    REQUIRE(contains(line, "\"targetFrameCount\":600"));
    REQUIRE(contains(line, "\"unavailablePrehistory\":2"));

    TelemetryRecord exit;
    exit.kind = RecordKind::lifecycle;
    exit.monotonicNs = 6;
    std::snprintf(exit.eventType, sizeof(exit.eventType), "%s", "runtime_exit");
    exit.aux = 700;
    const std::string exitLine = encoder.encode(2, exit);
    REQUIRE(contains(exitLine, "\"kind\":\"lifecycle\""));
    REQUIRE(contains(exitLine, "\"event\":\"runtime_exit\""));
    REQUIRE(contains(exitLine, "\"lastFrameIndex\":700"));
}
