#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "YoghourtTelemetryCollector.h"

#include <atomic>
#include <string>
#include <vector>

using namespace yoghourt_telemetry;

namespace {

std::atomic<uint64_t> gFakeNow{1'000'000};

uint64_t fakeNow() {
    return gFakeNow.load(std::memory_order_relaxed);
}

void advanceFakeClockMs(uint64_t ms) {
    gFakeNow.fetch_add(ms * 1'000'000ull, std::memory_order_relaxed);
}

class CapturingSink final : public OutputSink {
public:
    void writeLine(const char *data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.emplace_back(data, size);
    }

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

uint64_t extractUint(const std::string &line, const char *key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = line.find(needle);
    if (at == std::string::npos) return 0;
    const size_t start = at + needle.size();
    size_t end = start;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
    return std::stoull(line.substr(start, end - start));
}

size_t countLines(const std::vector<std::string> &lines, const char *kind) {
    const std::string needle = std::string("\"kind\":\"") + kind + "\"";
    size_t count = 0;
    for (const auto &line : lines) {
        if (line.find(needle) != std::string::npos) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("collector streams ordered lines with strict sequence and lifecycle records", "[telemetry][collector]") {
    auto sink = std::make_unique<CapturingSink>();
    CapturingSink *sinkPtr = sink.get();
    TelemetryCollector collector("sess-1", std::move(sink), &fakeNow);

    for (uint64_t i = 1; i <= 50; ++i) {
        advanceFakeClockMs(17);
        const uint64_t index = collector.onFrameSubmitted();
        REQUIRE(index == i);
        advanceFakeClockMs(1);
        collector.onFrameCompleted(index, 4'000'000, true); // 4 ms
        if (i % 2 == 0) {
            collector.onFramePresented(index);
        }
    }
    collector.shutdown(TelemetryCollector::kShutdownDrainTimeoutMs);

    const auto lines = sinkPtr->lines();
    // Ordinary steady-state frames stay in the runtime ring; only summaries
    // and anomaly/burst ranges cross the stdout side-channel.
    REQUIRE(countLines(lines, "frame") == 0);
    REQUIRE(countLines(lines, "lifecycle") == 2); // collector_start + runtime_exit

    // Every line: reserved prefix, envelope, this session's ID.
    for (const auto &line : lines) {
        REQUIRE(line.find("[YOGHOURT_TELEMETRY_V1]") == 0);
        REQUIRE(line.find("\"sessionID\":\"sess-1\"") != std::string::npos);
    }

    // Sequence is strictly increasing across the whole stream.
    uint64_t last = 0;
    for (const auto &line : lines) {
        const uint64_t sequence = extractUint(line, "sequence");
        REQUIRE(sequence == last + 1);
        last = sequence;
    }

    REQUIRE(collector.stopped());
    // Hooks after shutdown are no-ops (frameIndex 0 signals "not recorded").
    REQUIRE(collector.onFrameSubmitted() == 0);
}

TEST_CASE("collector emits snapshot and aggregate records on the injected clock", "[telemetry][collector]") {
    auto sink = std::make_unique<CapturingSink>();
    CapturingSink *sinkPtr = sink.get();
    TelemetryCollector collector("sess-2", std::move(sink), &fakeNow);

    // 1 s of frames at ~60 fps covers both the 250 ms snapshot and the 1 s
    // aggregate cadence.
    for (int i = 0; i < 61; ++i) {
        advanceFakeClockMs(17);
        const uint64_t frameIndex = collector.onFrameSubmitted();
        collector.onFrameStages(frameIndex, 2.0, 0.75, 16.7);
    }
    collector.shutdown(TelemetryCollector::kShutdownDrainTimeoutMs);

    const auto lines = sinkPtr->lines();
    const size_t snapshots = countLines(lines, "snapshot");
    const size_t aggregates = countLines(lines, "aggregate");
    REQUIRE(snapshots >= 3);
    REQUIRE(aggregates >= 1);

    bool sawStageWindow = false;
    for (const auto &line : lines) {
        if (line.find("\"kind\":\"snapshot\"") == std::string::npos &&
            line.find("\"kind\":\"aggregate\"") == std::string::npos) {
            continue;
        }
        REQUIRE(line.find("\"intervalCount\"") != std::string::npos);
        REQUIRE(line.find("\"stageCount\"") != std::string::npos);
        if (extractUint(line, "stageCount") == 0) continue;
        sawStageWindow = true;
        REQUIRE(line.find("\"tickP50Ms\":2.000") != std::string::npos);
        REQUIRE(line.find("\"renderP50Ms\":0.750") != std::string::npos);
        REQUIRE(line.find("\"swapP50Ms\":16.700") != std::string::npos);
        REQUIRE(line.find("\"gpuTimingLateDrop\":0") != std::string::npos);
        REQUIRE(line.find("\"telemetryDropped\":0") != std::string::npos);
    }
    REQUIRE(sawStageWindow);
}

TEST_CASE("burst drains prehistory rows and emits the burst record", "[telemetry][collector]") {
    auto sink = std::make_unique<CapturingSink>();
    CapturingSink *sinkPtr = sink.get();
    TelemetryCollector collector("sess-3", std::move(sink), &fakeNow);

    // Warm-up: 121 frames at ~16.7 ms leave the detector armed with a stable
    // baseline.
    for (int i = 0; i < 121; ++i) {
        advanceFakeClockMs(17);
        const uint64_t index = collector.onFrameSubmitted();
        collector.onFrameCompleted(index, 4'000'000, true);
    }
    // 5 consecutive anomalies at 2.4x: burst on the fifth.
    uint64_t triggerIndex = 0;
    for (int i = 0; i < 5; ++i) {
        advanceFakeClockMs(40);
        triggerIndex = collector.onFrameSubmitted();
        collector.onFrameCompleted(triggerIndex, 4'000'000, true);
    }
    REQUIRE(triggerIndex == 126);
    collector.shutdown(TelemetryCollector::kShutdownDrainTimeoutMs);

    const auto lines = sinkPtr->lines();
    size_t burstRows = 0;
    bool burstRecord = false;
    size_t burstMarkerPosition = lines.size();
    size_t firstBurstFramePosition = lines.size();
    for (size_t position = 0; position < lines.size(); ++position) {
        const auto &line = lines[position];
        if (line.find("\"reason\":\"burst\"") != std::string::npos) {
            ++burstRows;
            firstBurstFramePosition = std::min(firstBurstFramePosition, position);
        }
        if (line.find("\"kind\":\"burst\"") != std::string::npos) {
            burstRecord = true;
            burstMarkerPosition = std::min(burstMarkerPosition, position);
            REQUIRE(extractUint(line, "startFrameIndex") == 1);
            REQUIRE(extractUint(line, "endFrameIndex") == 126 + 299);
            REQUIRE(extractUint(line, "triggerFrameIndex") == 126);
            REQUIRE(extractUint(line, "targetFrameCount") == 600);
            REQUIRE(extractUint(line, "unavailablePrehistory") == 175);
        }
    }
    REQUIRE(burstRecord);
    REQUIRE(burstMarkerPosition < firstBurstFramePosition);
    // Prehistory [1, 125] drained from the ring; the trigger frame itself
    // streams from its completion.
    REQUIRE(burstRows >= 125);
}

TEST_CASE("ring busy drop events are coalesced", "[telemetry][collector]") {
    auto sink = std::make_unique<CapturingSink>();
    CapturingSink *sinkPtr = sink.get();
    TelemetryCollector collector("sess-4", std::move(sink), &fakeNow);

    for (int i = 0; i < 100; ++i) {
        advanceFakeClockMs(1);
        collector.onRingBusyDrop();
    }
    collector.shutdown(TelemetryCollector::kShutdownDrainTimeoutMs);

    const auto lines = sinkPtr->lines();
    REQUIRE(countLines(lines, "event") == 1);
    for (const auto &line : lines) {
        if (line.find("\"eventType\":\"ring_busy_drop\"") != std::string::npos) {
            REQUIRE(extractUint(line, "count") == 1);
        }
    }
}

TEST_CASE("late gpu completions are counted and never attached", "[telemetry][collector]") {
    auto sink = std::make_unique<CapturingSink>();
    CapturingSink *sinkPtr = sink.get();
    TelemetryCollector collector("sess-5", std::move(sink), &fakeNow);

    for (int i = 0; i < 10; ++i) {
        advanceFakeClockMs(17);
        const uint64_t index = collector.onFrameSubmitted();
        if (index != 1) {
            collector.onFrameCompleted(index, 4'000'000, true);
        }
    }
    // Push frame 1's slot out of the ring.
    for (int i = 0; i < static_cast<int>(kFrameRingCapacity); ++i) {
        advanceFakeClockMs(1);
        collector.onFrameSubmitted();
    }
    // Frame 1's completion arrives after its slot was overwritten: counted
    // as a late drop, never streamed as a row.
    collector.onFrameCompleted(1, 4'000'000, true);
    REQUIRE(collector.gpuTimingLateDrop() == 1);
    collector.shutdown(TelemetryCollector::kShutdownDrainTimeoutMs);

    const auto lines = sinkPtr->lines();
    for (const auto &line : lines) {
        if (line.find("\"kind\":\"frame\"") == std::string::npos) continue;
        REQUIRE(extractUint(line, "frameIndex") != 1);
    }
}
