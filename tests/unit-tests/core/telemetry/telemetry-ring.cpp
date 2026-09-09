#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "YoghourtTelemetryRing.h"

#include <atomic>
#include <thread>

using namespace yoghourt_telemetry;

namespace {

FrameSample makeSample(uint64_t frameIndex) {
    FrameSample sample;
    sample.frameIndex = frameIndex;
    sample.monotonicNs = frameIndex * 16666000ull;
    sample.engineIntervalMs = 16.666;
    sample.hasInterval = frameIndex > 1;
    return sample;
}

} // namespace

TEST_CASE("ring backfills gpu time for the exact frame", "[telemetry][ring]") {
    FrameSampleRing ring;
    for (uint64_t i = 1; i <= 10; ++i) {
        ring.write(makeSample(i));
    }
    FrameSample out;
    REQUIRE(ring.completeGpu(5, 4.5, true, out));
    REQUIRE(out.gpuFrameMs == Catch::Approx(4.5));
    REQUIRE(ring.read(5, out));
    REQUIRE(out.gpuFrameMs == Catch::Approx(4.5));
    REQUIRE(ring.gpuTimingLateDrop() == 0);
}

TEST_CASE("ring counts a late completion instead of mis-attaching", "[telemetry][ring]") {
    FrameSampleRing ring;
    for (uint64_t i = 1; i <= kFrameRingCapacity + 5; ++i) {
        ring.write(makeSample(i));
    }
    // Frame 1's slot was overwritten long ago.
    FrameSample out;
    REQUIRE_FALSE(ring.completeGpu(1, 4.5, true, out));
    REQUIRE(ring.gpuTimingLateDrop() == 1);
    // The overwritten slot now holds frame kFrameRingCapacity + 1 (index 1 mod capacity).
    REQUIRE(ring.read(kFrameRingCapacity + 1, out));
    REQUIRE(out.gpuFrameMs < 0.0);
}

TEST_CASE("ring keeps presented and reason flags across backfill", "[telemetry][ring]") {
    FrameSampleRing ring;
    ring.write(makeSample(1));
    ring.setReason(1, kFrameReasonAnomaly);
    ring.markPresented(1);
    FrameSample out;
    REQUIRE(ring.completeGpu(1, 2.0, true, out));
    REQUIRE(out.reason == kFrameReasonAnomaly);
    REQUIRE(out.presented);
}

TEST_CASE("ring backfills frame stage timings for the exact frame", "[telemetry][ring]") {
    FrameSampleRing ring;
    ring.write(makeSample(9));
    REQUIRE(ring.completeStages(9, 2.25, 0.75, 16.7));
    FrameSample out;
    REQUIRE(ring.read(9, out));
    REQUIRE(out.tickMs == Catch::Approx(2.25));
    REQUIRE(out.renderMs == Catch::Approx(0.75));
    REQUIRE(out.swapMs == Catch::Approx(16.7));
    REQUIRE_FALSE(ring.completeStages(10, 1.0, 1.0, 1.0));
}

TEST_CASE("gpu window exposes percentiles and count", "[telemetry][ring]") {
    FrameSampleRing ring;
    for (uint64_t i = 1; i <= 100; ++i) {
        ring.write(makeSample(i));
        FrameSample out;
        REQUIRE(ring.completeGpu(i, static_cast<double>(i) * 0.1, true, out));
    }
    const auto window = ring.gpuWindow();
    REQUIRE(window.count == 100);
    REQUIRE(window.maxMs == Catch::Approx(10.0));
    REQUIRE(window.p50Ms > 0.0);
    REQUIRE(window.lastMs == Catch::Approx(10.0));
}

TEST_CASE("gpu window only accepts successful positive timings", "[telemetry][ring]") {
    FrameSampleRing ring;
    ring.write(makeSample(1));
    FrameSample out;
    REQUIRE(ring.completeGpu(1, 0.0, true, out)); // zero duration: recorded, window untouched
    REQUIRE(ring.gpuWindow().count == 0);
    ring.write(makeSample(2));
    REQUIRE(ring.completeGpu(2, 3.0, false, out)); // failed: no window push
    REQUIRE(ring.gpuWindow().count == 0);
    ring.write(makeSample(3));
    REQUIRE(ring.completeGpu(3, 5.0, true, out));
    REQUIRE(ring.gpuWindow().count == 1);
}

TEST_CASE("concurrent backfill and submission stay consistent", "[telemetry][ring]") {
    FrameSampleRing ring;
    std::atomic<bool> stop{false};
    std::thread completer([&] {
        uint64_t index = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            if (index <= 2000) {
                ring.write(makeSample(index));
                FrameSample out;
                ring.completeGpu(index, 1.5, true, out);
                ++index;
            }
        }
    });
    for (uint64_t i = 1; i <= 2000; ++i) {
        FrameSample out;
        // Reads may legitimately miss (slot just overwritten), but the tag
        // check must never return a foreign frame's sample.
        if (ring.read(i, out)) {
            REQUIRE(out.frameIndex == i);
        }
    }
    stop.store(true);
    completer.join();
}
