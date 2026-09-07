#pragma once

#include "YoghourtTelemetryRecords.h"

#include <stdint.h>

namespace yoghourt_telemetry {

// Relative frame-interval anomaly detection, render thread only.
//
// Contract (plan §5): the first kWarmupIntervals valid intervals never raise
// anomalies; the baseline is the median of the most recent
// kIntervalWindowSize valid intervals, recomputed at most once per second;
// an interval above baseline × kAnomalyIntervalFactor is an anomaly;
// kBurstConsecutiveAnomalies consecutive anomalies trigger one burst (the
// detector re-arms only after kBurstRearmNormalIntervals consecutive normal
// intervals). An interval crossing a discontinuity (pause, rebuild, gap) is
// invalid: it is not pushed into the window, clears the window, and restarts
// warm-up from the next consecutive pair.
class AnomalyDetector {
public:
    struct Verdict {
        bool anomaly = false;
        bool burst = false;          // a NEW burst triggered on this frame
        bool discontinuity = false;  // interval crossed the pause/gap boundary
        bool warmup = false;         // still inside the warm-up budget
        uint64_t triggerFrameIndex = 0;
        uint32_t consecutiveAnomalies = 0;
    };

    struct WindowStats {
        uint32_t count = 0;
        double p50Ms = 0.0;
        double p99Ms = 0.0;
        double maxMs = 0.0;
        double fps = 0.0;
    };

    // nowNs drives the once-per-second median recompute and must share the
    // MonotonicNs timebase.
    Verdict onFrame(uint64_t frameIndex, bool hasInterval, double intervalMs, uint64_t nowNs);

    WindowStats window() const;

    // Clears the window and restarts warm-up (resolution rebuild, explicit
    // discontinuity). Frame indices are not touched.
    void reset();

    bool armed() const { return armed_; }

private:
    void pushInterval(double ms);
    void maybeRecomputeMedian(uint64_t nowNs);

    double intervals_[kIntervalWindowSize] = {};
    size_t intervalCount_ = 0;
    size_t intervalCursor_ = 0;

    size_t warmupRemaining_ = kWarmupIntervals;
    double cachedMedianMs_ = 0.0;
    uint64_t lastMedianRecomputeNs_ = 0;
    uint64_t lastValidNs_ = 0;

    bool armed_ = true;
    size_t consecutiveAnomalies_ = 0;
    size_t consecutiveNormals_ = 0;
};

} // namespace yoghourt_telemetry
