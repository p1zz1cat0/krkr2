#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "YoghourtAnomalyDetector.h"

using namespace yoghourt_telemetry;

namespace {

constexpr uint64_t kNs = 1;

} // namespace

TEST_CASE("warm-up suppresses anomalies", "[telemetry][anomaly]") {
    AnomalyDetector detector;
    uint64_t now = 0;
    // 60 valid intervals (frames 2..61), then one huge interval: still inside
    // warm-up. Frame 1 has no interval and reports neither warm-up nor
    // anomaly.
    for (uint64_t i = 1; i <= 61; ++i) {
        now += 16666 * kNs;
        const auto verdict = detector.onFrame(i, i > 1, 16.666, now);
        REQUIRE_FALSE(verdict.anomaly);
        if (i > 1) {
            REQUIRE(verdict.warmup);
        }
    }
    now += 30000 * kNs; // ~2x baseline, warm-up still active (60 of 120 used)
    const auto verdict = detector.onFrame(62, true, 30.0, now);
    REQUIRE_FALSE(verdict.anomaly);
    REQUIRE(verdict.warmup);
}

TEST_CASE("interval above 1.5x median is an anomaly after warm-up", "[telemetry][anomaly]") {
    AnomalyDetector detector;
    uint64_t now = 0;
    AnomalyDetector::Verdict verdict;
    for (uint64_t i = 1; i <= 121; ++i) {
        now += 16666 * kNs;
        verdict = detector.onFrame(i, i > 1, 16.666, now);
    }
    REQUIRE_FALSE(verdict.warmup);
    now += 30000 * kNs;
    verdict = detector.onFrame(122, true, 30.0, now);
    REQUIRE(verdict.anomaly);
    REQUIRE(verdict.consecutiveAnomalies == 1);
    REQUIRE_FALSE(verdict.burst);
}

TEST_CASE("burst triggers on the fifth consecutive anomaly and re-arms after 30 normals", "[telemetry][anomaly]") {
    AnomalyDetector detector;
    uint64_t now = 0;
    for (uint64_t i = 1; i <= 121; ++i) {
        now += 16666 * kNs;
        (void)detector.onFrame(i, i > 1, 16.666, now);
    }
    bool burstSeen = false;
    for (uint64_t i = 122; i <= 126; ++i) {
        now += 40000 * kNs; // sustained 2.4x
        const auto verdict = detector.onFrame(i, true, 40.0, now);
        if (i < 126) {
            REQUIRE(verdict.anomaly);
            REQUIRE_FALSE(verdict.burst);
        } else {
            REQUIRE(verdict.burst);
            REQUIRE(verdict.triggerFrameIndex == 126);
            burstSeen = true;
        }
    }
    REQUIRE(burstSeen);
    REQUIRE_FALSE(detector.armed());

    // Burst period: anomalies alone do not re-trigger.
    for (uint64_t i = 127; i <= 140; ++i) {
        now += 40000 * kNs;
        const auto verdict = detector.onFrame(i, true, 40.0, now);
        REQUIRE_FALSE(verdict.burst);
    }
    // 30 consecutive normals re-arm.
    for (uint64_t i = 141; i <= 170; ++i) {
        now += 16666 * kNs;
        const auto verdict = detector.onFrame(i, true, 16.666, now);
        REQUIRE_FALSE(verdict.burst);
        REQUIRE_FALSE(verdict.anomaly);
    }
    REQUIRE(detector.armed());
}

TEST_CASE("interval crossing a discontinuity is invalid and clears the window", "[telemetry][anomaly]") {
    AnomalyDetector detector;
    uint64_t now = 0;
    for (uint64_t i = 1; i <= 200; ++i) {
        now += 16666 * kNs;
        (void)detector.onFrame(i, i > 1, 16.666, now);
    }
    REQUIRE(detector.window().count == 120);

    now += 2 * 1000 * 1000 * kNs; // 2 s pause
    const auto verdict = detector.onFrame(201, true, 2000.0, now);
    REQUIRE(verdict.discontinuity);
    REQUIRE_FALSE(verdict.anomaly);
    REQUIRE(detector.window().count == 0);

    // Warm-up restarted: a spike right after the gap must not raise an anomaly.
    now += 16666 * kNs;
    (void)detector.onFrame(202, true, 16.666, now);
    now += 30000 * kNs;
    const auto spike = detector.onFrame(203, true, 30.0, now);
    REQUIRE(spike.warmup);
    REQUIRE_FALSE(spike.anomaly);
}

TEST_CASE("window stats expose percentiles and fps", "[telemetry][anomaly]") {
    AnomalyDetector detector;
    uint64_t now = 0;
    for (uint64_t i = 1; i <= 121; ++i) {
        now += 16666 * kNs;
        (void)detector.onFrame(i, i > 1, 16.666, now);
    }
    const auto stats = detector.window();
    REQUIRE(stats.count == 120);
    REQUIRE(stats.p50Ms == Catch::Approx(16.666).margin(0.001));
    REQUIRE(stats.maxMs == Catch::Approx(16.666).margin(0.001));
    REQUIRE(stats.fps == Catch::Approx(60.0).margin(1.0));
}

TEST_CASE("first frame has no interval and never counts as anomaly", "[telemetry][anomaly]") {
    AnomalyDetector detector;
    const auto verdict = detector.onFrame(1, false, 0.0, 1000);
    REQUIRE_FALSE(verdict.anomaly);
    REQUIRE_FALSE(verdict.discontinuity);
}
