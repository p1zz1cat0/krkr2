#include "YoghourtTelemetryCollector.h"

#include "YoghourtTelemetryClock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace yoghourt_telemetry {

namespace {

double percentileOf(const double *sorted, size_t count, double fraction) {
    if (count == 0) return 0.0;
    const size_t index = static_cast<size_t>(static_cast<double>(count - 1) * fraction);
    return sorted[index];
}

} // namespace

void StdoutOutputSink::writeLine(const char *data, size_t size) {
    // One stdio call per line: stdio is internally locked per call on
    // Apple/Linux, so a telemetry line never interleaves with the runtime's
    // other stdout writers mid-line.
    std::fwrite(data, 1, size, stdout);
    std::fflush(stdout);
}

TelemetryCollector::TelemetryCollector(const char *sessionID, std::unique_ptr<OutputSink> sink, uint64_t (*clockOverride)())
    : queue_(), ring_(), detector_(), encoder_(sessionID), sink_(std::move(sink)), clockOverride_(clockOverride) {
    TelemetryRecord start;
    start.kind = RecordKind::lifecycle;
    start.monotonicNs = nowNs();
    std::snprintf(start.eventType, sizeof(start.eventType), "%s", "collector_start");
    emitRecord(start);

    worker_ = std::thread(&TelemetryCollector::workerLoop, this);
}

TelemetryCollector::~TelemetryCollector() {
    if (!shutdownCalled_.load(std::memory_order_acquire)) {
        shutdown(kShutdownDrainTimeoutMs);
    }
}

void TelemetryCollector::emitRecord(const TelemetryRecord &record) {
    queue_.push(record);
    // notify_one() does not require owning the mutex. Avoid taking a render
    // hook mutex merely to wake the worker; the worker's timed wait also
    // covers a notify that races with the wait transition.
    wakeCv_.notify_one();
}

void TelemetryCollector::workerLoop() {
    TelemetryRecord record;
    for (;;) {
        for (;;) {
            if (!queue_.pop(record)) break;
            const uint64_t sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            const std::string line = encoder_.encode(sequence, record);
            sink_->writeLine(line.data(), line.size());
            linesEmitted_.fetch_add(1, std::memory_order_relaxed);
        }
        if (stopped_.load(std::memory_order_acquire)) {
            // Final drain: records already enqueued (including the
            // runtime_exit lifecycle record) leave before the worker exits.
            // A completion callback that races past its stopped check may
            // still land one record here; it is bounded, never blocks, and
            // the process is exiting.
            while (queue_.pop(record)) {
                const uint64_t sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
                const std::string line = encoder_.encode(sequence, record);
                sink_->writeLine(line.data(), line.size());
                linesEmitted_.fetch_add(1, std::memory_order_relaxed);
            }
            workerExited_.store(true, std::memory_order_release);
            return;
        }
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wakeCv_.wait_for(lock, std::chrono::milliseconds(2));
    }
}

uint64_t TelemetryCollector::onFrameSubmitted() {
    if (stopped_.load(std::memory_order_acquire)) return 0;
    const uint64_t now = nowNs();
    const uint64_t index = nextFrameIndex_++;
    double intervalMs = 0.0;
    const bool hasInterval = lastSubmitNs_ != 0;
    if (hasInterval) intervalMs = static_cast<double>(now - lastSubmitNs_) / 1e6;
    lastSubmitNs_ = now;

    const auto verdict = detector_.onFrame(index, hasInterval, intervalMs, now);

    FrameSample sample;
    sample.frameIndex = index;
    sample.monotonicNs = now;
    sample.engineIntervalMs = intervalMs;
    sample.hasInterval = hasInterval && !verdict.discontinuity;
    sample.telemetryDropped = queue_.normalDropped();
    sample.criticalTelemetryDropped = queue_.criticalDropped();
    ring_.write(sample);

    if (verdict.anomaly || verdict.burst) {
        ring_.setReason(index, verdict.burst ? kFrameReasonBurst : kFrameReasonAnomaly);
    }
    if (verdict.burst) {
        emitBurstPrehistory(index, now);
    }
    emitPeriodic(now);
    return index;
}

void TelemetryCollector::onFrameStages(uint64_t frameIndex, double tickMs, double renderMs, double swapMs) {
    if (frameIndex == 0 || stopped_.load(std::memory_order_acquire)) return;
    if (!std::isfinite(tickMs) || !std::isfinite(renderMs) || !std::isfinite(swapMs) ||
        tickMs < 0.0 || renderMs < 0.0 || swapMs < 0.0) {
        return;
    }
    if (!ring_.completeStages(frameIndex, tickMs, renderMs, swapMs)) return;
    pushStageSample(tickMs, renderMs, swapMs);
}

void TelemetryCollector::pushStageSample(double tickMs, double renderMs, double swapMs) {
    tickSamples_[stageSampleCursor_] = tickMs;
    renderSamples_[stageSampleCursor_] = renderMs;
    swapSamples_[stageSampleCursor_] = swapMs;
    stageSampleCursor_ = (stageSampleCursor_ + 1) % kIntervalWindowSize;
    stageSampleCount_ = std::min(stageSampleCount_ + 1, kIntervalWindowSize);
}

TelemetryCollector::StageWindowSnapshot TelemetryCollector::stageWindow() const {
    double tick[kIntervalWindowSize];
    double render[kIntervalWindowSize];
    double swap[kIntervalWindowSize];
    std::memcpy(tick, tickSamples_, sizeof(double) * stageSampleCount_);
    std::memcpy(render, renderSamples_, sizeof(double) * stageSampleCount_);
    std::memcpy(swap, swapSamples_, sizeof(double) * stageSampleCount_);
    std::sort(tick, tick + stageSampleCount_);
    std::sort(render, render + stageSampleCount_);
    std::sort(swap, swap + stageSampleCount_);

    StageWindowSnapshot snapshot;
    snapshot.count = static_cast<uint32_t>(stageSampleCount_);
    snapshot.tickP50Ms = percentileOf(tick, stageSampleCount_, 0.50);
    snapshot.tickP99Ms = percentileOf(tick, stageSampleCount_, 0.99);
    snapshot.tickMaxMs = stageSampleCount_ > 0 ? tick[stageSampleCount_ - 1] : 0.0;
    snapshot.renderP50Ms = percentileOf(render, stageSampleCount_, 0.50);
    snapshot.renderP99Ms = percentileOf(render, stageSampleCount_, 0.99);
    snapshot.renderMaxMs = stageSampleCount_ > 0 ? render[stageSampleCount_ - 1] : 0.0;
    snapshot.swapP50Ms = percentileOf(swap, stageSampleCount_, 0.50);
    snapshot.swapP99Ms = percentileOf(swap, stageSampleCount_, 0.99);
    snapshot.swapMaxMs = stageSampleCount_ > 0 ? swap[stageSampleCount_ - 1] : 0.0;
    return snapshot;
}

void TelemetryCollector::onFramePresented(uint64_t frameIndex) {
    if (frameIndex == 0 || stopped_.load(std::memory_order_acquire)) return;
    ring_.markPresented(frameIndex);
}

void TelemetryCollector::onFrameCompleted(uint64_t frameIndex, uint64_t gpuDurationNs, bool succeeded) {
    if (frameIndex == 0 || stopped_.load(std::memory_order_acquire)) return;
    const double gpuMs = static_cast<double>(gpuDurationNs) / 1e6;
    FrameSample sample;
    if (!ring_.completeGpu(frameIndex, gpuMs, succeeded, sample)) {
        // The slot was overwritten before completion arrived: severe signal
        // (GPU stall / queue overrun), never silently attached elsewhere.
        warnGpuLateOnce();
        return;
    }
    // Lagged streaming: emit only anomaly/burst-range frames. Ordinary
    // samples remain in the runtime ring and are summarized at 4 Hz; this
    // keeps the stdout side-channel out of the steady-state frame path.
    if (frameIndex > 1) {
        tryStreamFrame(frameIndex - 1);
    }
    const uint64_t burstEnd = activeBurstEndFrame_.load(std::memory_order_acquire);
    if (burstEnd != 0 && frameIndex > burstEnd + 1) {
        activeBurstEndFrame_.store(0, std::memory_order_release);
    }
}

void TelemetryCollector::tryStreamFrame(uint64_t frameIndex, bool allowIncomplete) {
    FrameSample sample;
    if (!ring_.read(frameIndex, sample)) return;
    const uint64_t burstEnd = activeBurstEndFrame_.load(std::memory_order_acquire);
    const bool inBurst = burstEnd != 0 && frameIndex <= burstEnd;
    if (sample.reason == kFrameReasonNone && !inBurst) return;
    if (!ring_.takeForStreaming(frameIndex, sample, allowIncomplete)) return;
    TelemetryRecord record;
    record.kind = RecordKind::frame;
    record.monotonicNs = sample.monotonicNs;
    record.frame = sample;
    emitRecord(record);
}

void TelemetryCollector::onOutputRebuild(uint32_t oldWidth, uint32_t oldHeight, uint32_t newWidth, uint32_t newHeight) {
    if (stopped_.load(std::memory_order_acquire)) return;
    detector_.reset();
    TelemetryRecord record;
    record.kind = RecordKind::event;
    record.monotonicNs = nowNs();
    std::snprintf(record.eventType, sizeof(record.eventType), "%s", "resolution_change");
    std::snprintf(record.detail, sizeof(record.detail), "%ux%u->%ux%u", oldWidth, oldHeight, newWidth, newHeight);
    emitRecord(record);
}

void TelemetryCollector::onRendererSelected(const char *renderer,
                                            bool accurateRender) {
    if (rendererRecorded_.exchange(true, std::memory_order_relaxed)) return;
    TelemetryRecord record;
    record.kind = RecordKind::lifecycle;
    record.monotonicNs = nowNs();
    std::snprintf(record.eventType, sizeof(record.eventType), "%s", "renderer_selected");
    std::snprintf(record.detail, sizeof(record.detail), "renderer=%s accurate=%d",
                  renderer && *renderer ? renderer : "unknown", accurateRender ? 1 : 0);
    emitRecord(record);
}

void TelemetryCollector::onPipelineFailure(const char *reason) {
    if (stopped_.load(std::memory_order_acquire)) return;
    TelemetryRecord record;
    record.kind = RecordKind::event;
    record.monotonicNs = nowNs();
    std::snprintf(record.eventType, sizeof(record.eventType), "%s", "pipeline_failure");
    if (reason) std::snprintf(record.detail, sizeof(record.detail), "%s", reason);
    emitRecord(record);
}

void TelemetryCollector::onRingBusyDrop() {
    if (stopped_.load(std::memory_order_acquire)) return;
    const uint64_t now = nowNs();
    ++coalescedRingBusyDrops_;
    if (lastRingBusyEventNs_ != 0 && now - lastRingBusyEventNs_ < kRingBusyDropCoalesceMs * 1000000ull) {
        return;
    }
    lastRingBusyEventNs_ = now;
    TelemetryRecord record;
    record.kind = RecordKind::event;
    record.monotonicNs = now;
    std::snprintf(record.eventType, sizeof(record.eventType), "%s", "ring_busy_drop");
    record.aux = coalescedRingBusyDrops_;
    coalescedRingBusyDrops_ = 0;
    emitRecord(record);
}

void TelemetryCollector::emitPeriodic(uint64_t nowNs) {
    const uint64_t snapshotStep = kSnapshotIntervalMs * 1000000ull;
    const uint64_t aggregateStep = kAggregateIntervalMs * 1000000ull;
    const bool snapshotDue = lastSnapshotNs_ == 0 || nowNs - lastSnapshotNs_ >= snapshotStep;
    const bool aggregateDue = lastAggregateNs_ == 0 || nowNs - lastAggregateNs_ >= aggregateStep;
    if (!snapshotDue && !aggregateDue) return;

    const auto intervalStats = detector_.window();
    const auto gpuStats = ring_.gpuWindow();
    const auto stageStats = stageWindow();

    const auto fill = [&](TelemetryRecord &record) {
        record.monotonicNs = nowNs;
        record.stats.fps = intervalStats.fps;
        record.stats.intervalCount = intervalStats.count;
        record.stats.intervalP50Ms = intervalStats.p50Ms;
        record.stats.intervalP99Ms = intervalStats.p99Ms;
        record.stats.intervalMaxMs = intervalStats.maxMs;
        record.stats.gpuCount = gpuStats.count;
        record.stats.gpuP50Ms = gpuStats.p50Ms;
        record.stats.gpuP99Ms = gpuStats.p99Ms;
        record.stats.gpuMaxMs = gpuStats.maxMs;
        record.stats.lastGpuFrameMs = gpuStats.lastMs;
        record.stats.stageCount = stageStats.count;
        record.stats.tickP50Ms = stageStats.tickP50Ms;
        record.stats.tickP99Ms = stageStats.tickP99Ms;
        record.stats.tickMaxMs = stageStats.tickMaxMs;
        record.stats.renderP50Ms = stageStats.renderP50Ms;
        record.stats.renderP99Ms = stageStats.renderP99Ms;
        record.stats.renderMaxMs = stageStats.renderMaxMs;
        record.stats.swapP50Ms = stageStats.swapP50Ms;
        record.stats.swapP99Ms = stageStats.swapP99Ms;
        record.stats.swapMaxMs = stageStats.swapMaxMs;
        record.stats.gpuTimingLateDrop = ring_.gpuTimingLateDrop();
        record.stats.telemetryDropped = queue_.normalDropped();
        record.stats.criticalTelemetryDropped = queue_.criticalDropped();
    };

    if (snapshotDue) {
        TelemetryRecord record;
        record.kind = RecordKind::snapshot;
        fill(record);
        emitRecord(record);
        lastSnapshotNs_ = nowNs;
    }
    if (aggregateDue) {
        TelemetryRecord record;
        record.kind = RecordKind::aggregate;
        fill(record);
        emitRecord(record);
        lastAggregateNs_ = nowNs;
    }
}

void TelemetryCollector::emitBurstPrehistory(uint64_t triggerIndex, uint64_t nowNs) {
    const uint64_t lower = triggerIndex > kBurstPrehistoryFrames ? triggerIndex - kBurstPrehistoryFrames : 1;
    // The conceptual range is [T-300, T+299]. Frame indices start at 1, so
    // indices before 1 are unavailable history rather than a writer gap.
    uint64_t missing = triggerIndex > kBurstPrehistoryFrames
        ? 0
        : kBurstPrehistoryFrames - (triggerIndex - 1);
    // Probe first, then publish the marker before any history rows. The host
    // must have an open burst before it sees the intentionally duplicated
    // anomaly rows; otherwise its final duplicate/new counts are incomplete.
    for (uint64_t index = lower; index < triggerIndex; ++index) {
        FrameSample sample;
        if (!ring_.read(index, sample)) {
            ++missing;
        }
    }

    TelemetryRecord record;
    record.kind = RecordKind::burst;
    record.monotonicNs = nowNs;
    std::snprintf(record.eventType, sizeof(record.eventType), "%s", "frame_drop_burst");
    record.startFrameIndex = lower;
    record.endFrameIndex = triggerIndex + kBurstPendingFutureFrames;
    record.triggerFrameIndex = triggerIndex;
    record.targetFrameCount = kBurstPrehistoryFrames + 1 + kBurstPendingFutureFrames;
    record.aux = missing;
    activeBurstStartFrame_.store(lower, std::memory_order_release);
    activeBurstEndFrame_.store(record.endFrameIndex, std::memory_order_release);
    emitRecord(record);

    for (uint64_t index = lower; index < triggerIndex; ++index) {
        FrameSample sample;
        // A frame still in flight is left for its completion callback, which
        // supplies the final GPU time while the burst remains open.
        if (ring_.takeForBurst(index, sample)) {
            sample.reason = kFrameReasonBurst;
            TelemetryRecord frameRecord;
            frameRecord.kind = RecordKind::frame;
            frameRecord.monotonicNs = sample.monotonicNs;
            frameRecord.frame = sample;
            emitRecord(frameRecord);
        }
    }
}

void TelemetryCollector::warnGpuLateOnce() {
    if (!gpuLateWarned_.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[Yoghourt] telemetry: gpu timing backfill late drop (sample overwritten); "
                     "investigate GPU stall, submission queue, or completion ownership\n");
    }
}

void TelemetryCollector::shutdown(uint32_t drainTimeoutMs) {
    bool expected = false;
    if (!shutdownCalled_.compare_exchange_strong(expected, true)) return;

    // Stream the trailing frame(s) the lagged completer never got to. A
    // shutdown-only incomplete sample keeps an anomaly visible when a GPU
    // completion never arrives; its gpuFrameMs remains unavailable.
    const uint64_t lastFrameIndex = nextFrameIndex_ - 1;
    const uint64_t burstStart = activeBurstStartFrame_.load(std::memory_order_acquire);
    const uint64_t burstEnd = activeBurstEndFrame_.load(std::memory_order_acquire);
    if (burstStart != 0 && burstEnd >= burstStart) {
        const uint64_t upper = std::min(lastFrameIndex, burstEnd);
        for (uint64_t index = burstStart; index <= upper; ++index) {
            tryStreamFrame(index, true);
        }
    } else {
        tryStreamFrame(lastFrameIndex, true);
    }

    TelemetryRecord exitRecord;
    exitRecord.kind = RecordKind::lifecycle;
    exitRecord.monotonicNs = nowNs();
    std::snprintf(exitRecord.eventType, sizeof(exitRecord.eventType), "%s", "runtime_exit");
    exitRecord.aux = lastFrameIndex;
    emitRecord(exitRecord);

    stopped_.store(true, std::memory_order_release);
    wakeCv_.notify_all();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drainTimeoutMs);
    bool drainWarningEmitted = false;
    while (!workerExited_.load(std::memory_order_acquire)) {
        if (!drainWarningEmitted && std::chrono::steady_clock::now() >= deadline) {
            // Do not detach: the worker still references this collector. A
            // detached worker would make stack-owned/test collectors use
            // after-free, and a leaked production collector would hide the
            // same ownership bug. The deadline is a diagnostic threshold;
            // safety requires joining before this object can be destroyed.
            std::fprintf(stderr,
                         "[Yoghourt] telemetry: shutdown drain exceeded %u ms; waiting for safe worker join\n",
                         drainTimeoutMs);
            drainWarningEmitted = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace yoghourt_telemetry
