#include "YoghourtAnomalyDetector.h"

#include <algorithm>
#include <cmath>

namespace yoghourt_telemetry {

namespace {

double percentileOf(double *sorted, size_t count, double fraction) {
    if (count == 0) return 0.0;
    const size_t index = static_cast<size_t>(static_cast<double>(count - 1) * fraction);
    return sorted[index];
}

} // namespace

void AnomalyDetector::reset() {
    intervalCount_ = 0;
    intervalCursor_ = 0;
    warmupRemaining_ = kWarmupIntervals;
    cachedMedianMs_ = 0.0;
    consecutiveAnomalies_ = 0;
    consecutiveNormals_ = 0;
    // armed_ survives: an active burst keeps its re-arm rule across rebuilds.
}

void AnomalyDetector::pushInterval(double ms) {
    intervals_[intervalCursor_] = ms;
    intervalCursor_ = (intervalCursor_ + 1) % kIntervalWindowSize;
    intervalCount_ = std::min(intervalCount_ + 1, kIntervalWindowSize);
}

void AnomalyDetector::maybeRecomputeMedian(uint64_t nowNs) {
    if (lastMedianRecomputeNs_ != 0 && nowNs - lastMedianRecomputeNs_ < kAggregateIntervalMs * 1000000ull) {
        return;
    }
    lastMedianRecomputeNs_ = nowNs;
    if (intervalCount_ == 0) {
        cachedMedianMs_ = 0.0;
        return;
    }
    double sorted[kIntervalWindowSize];
    for (size_t i = 0; i < intervalCount_; ++i) {
        sorted[i] = intervals_[i];
    }
    std::sort(sorted, sorted + intervalCount_);
    cachedMedianMs_ = percentileOf(sorted, intervalCount_, 0.50);
}

AnomalyDetector::Verdict AnomalyDetector::onFrame(uint64_t frameIndex, bool hasInterval, double intervalMs, uint64_t nowNs) {
    Verdict verdict;

    if (!hasInterval) {
        // First frame of the session (or after an explicit invalidation):
        // nothing to compare yet.
        return verdict;
    }

    if (intervalMs > kDiscontinuityIntervalMs || !std::isfinite(intervalMs) || intervalMs < 0.0) {
        // The interval crossing the gap is invalid by definition: drop it,
        // clear the baseline window, and restart warm-up from the next
        // consecutive pair.
        verdict.discontinuity = true;
        intervalCount_ = 0;
        intervalCursor_ = 0;
        warmupRemaining_ = kWarmupIntervals;
        cachedMedianMs_ = 0.0;
        consecutiveAnomalies_ = 0;
        consecutiveNormals_ = 0;
        return verdict;
    }

    if (warmupRemaining_ > 0) {
        --warmupRemaining_;
    }

    pushInterval(intervalMs);
    maybeRecomputeMedian(nowNs);

    const bool warmup = warmupRemaining_ > 0;
    verdict.warmup = warmup;

    const bool anomaly = !warmup && cachedMedianMs_ > 0.0 && intervalMs > cachedMedianMs_ * kAnomalyIntervalFactor;
    if (anomaly) {
        verdict.anomaly = true;
        ++consecutiveAnomalies_;
        consecutiveNormals_ = 0;
    } else {
        ++consecutiveNormals_;
        consecutiveAnomalies_ = 0;
    }
    verdict.consecutiveAnomalies = static_cast<uint32_t>(consecutiveAnomalies_);

    if (anomaly && armed_ && consecutiveAnomalies_ >= kBurstConsecutiveAnomalies) {
        verdict.burst = true;
        verdict.triggerFrameIndex = frameIndex;
        armed_ = false;
        consecutiveAnomalies_ = 0;
        consecutiveNormals_ = 0;
    } else if (!armed_ && consecutiveNormals_ >= kBurstRearmNormalIntervals) {
        armed_ = true;
    }

    return verdict;
}

AnomalyDetector::WindowStats AnomalyDetector::window() const {
    double sorted[kIntervalWindowSize];
    double sum = 0.0;
    for (size_t i = 0; i < intervalCount_; ++i) {
        sorted[i] = intervals_[i];
        sum += intervals_[i];
    }
    std::sort(sorted, sorted + intervalCount_);
    WindowStats stats;
    stats.count = static_cast<uint32_t>(intervalCount_);
    stats.p50Ms = percentileOf(sorted, intervalCount_, 0.50);
    stats.p99Ms = percentileOf(sorted, intervalCount_, 0.99);
    stats.maxMs = intervalCount_ > 0 ? sorted[intervalCount_ - 1] : 0.0;
    stats.fps = (intervalCount_ > 0 && sum > 0.0) ? static_cast<double>(intervalCount_) / (sum / 1000.0) : 0.0;
    return stats;
}

} // namespace yoghourt_telemetry
