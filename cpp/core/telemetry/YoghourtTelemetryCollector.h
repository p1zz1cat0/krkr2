#pragma once

#include "YoghourtAnomalyDetector.h"
#include "YoghourtTelemetryClock.h"
#include "YoghourtTelemetryEncoder.h"
#include "YoghourtTelemetryQueue.h"
#include "YoghourtTelemetryRing.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace yoghourt_telemetry {

// Destination for encoded telemetry lines. The production sink writes each
// line with a single stdio call (line-atomicity against other stdout
// writers); tests capture lines in memory.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void writeLine(const char *data, size_t size) = 0;
};

class StdoutOutputSink final : public OutputSink {
public:
    void writeLine(const char *data, size_t size) override;
};

// KrKr2 session telemetry collector: owns the frame ring, the anomaly
// detector, the bounded record queue, and the single worker thread that
// serializes records to the output sink.
//
// Threading: onFrameSubmitted/onOutputRebuild/onPipelineFailure/onRingBusyDrop
// run on the render thread; onFramePresented/onFrameCompleted run on Metal's
// present/completion queues. No hook blocks, formats, or touches files.
// shutdown() is called once from the runtime shutdown chain; hooks after it
// are cheap no-ops. The instance must outlive every
// in-flight command buffer — in production it is intentionally leaked at
// process exit (see the adapter), the same ownership shape as the
// presenter's async-failure signal.
class TelemetryCollector {
public:
    static constexpr uint32_t kShutdownDrainTimeoutMs = 250;

    // `clockOverride` replaces MonotonicNs() for every timestamp the
    // collector produces (tests inject a deterministic clock); null uses the
    // real monotonic timebase.
    TelemetryCollector(const char *sessionID, std::unique_ptr<OutputSink> sink, uint64_t (*clockOverride)() = nullptr);
    ~TelemetryCollector();

    TelemetryCollector(const TelemetryCollector &) = delete;
    TelemetryCollector &operator=(const TelemetryCollector &) = delete;

    // Returns the frame index for this submission, or 0 when the collector
    // is already stopped (hooks then ignore the matching callbacks).
    uint64_t onFrameSubmitted();
    void onFrameStages(uint64_t frameIndex, double tickMs, double renderMs, double swapMs);
    void onFramePresented(uint64_t frameIndex);
    void onFrameCompleted(uint64_t frameIndex, uint64_t gpuDurationNs, bool succeeded);
    void onOutputRebuild(uint32_t oldWidth, uint32_t oldHeight, uint32_t newWidth, uint32_t newHeight);
    /// 记录本次会话生效的图层合成 render manager。A/B 对比 software 与
    /// opengl 时，performance.csv 自身不含渲染路径信息，缺这条记录就无法
    /// 判断一组数字属于哪条路径。重复调用只发一次。
    void onRendererSelected(const char *renderer, bool accurateRender);
    void onPipelineFailure(const char *reason);
    // Sustained saturation is coalesced to at most one event per
    // kRingBusyDropCoalesceMs with the suppressed count attached.
    void onRingBusyDrop();

    // Emits runtime_exit, drains the queue, and joins the worker. The timeout
    // is a diagnostic threshold for a slow sink; the worker is never detached
    // because it still references this collector and must be joined before
    // the collector can be destroyed.
    void shutdown(uint32_t drainTimeoutMs);

    // Inspection counters (tests and diagnostics).
    uint64_t normalDropped() const { return queue_.normalDropped(); }
    uint64_t criticalDropped() const { return queue_.criticalDropped(); }
    uint64_t gpuTimingLateDrop() const { return ring_.gpuTimingLateDrop(); }
    uint64_t linesEmitted() const { return linesEmitted_.load(std::memory_order_relaxed); }
    uint64_t lastFrameIndex() const { return nextFrameIndex_ - 1; }
    bool stopped() const { return stopped_.load(std::memory_order_acquire); }

private:
    void workerLoop();
    void emitRecord(const TelemetryRecord &record);
    void emitPeriodic(uint64_t nowNs);
    void emitBurstPrehistory(uint64_t triggerIndex, uint64_t nowNs);
    void tryStreamFrame(uint64_t frameIndex, bool allowIncomplete = false);
    void warnGpuLateOnce();
    void pushStageSample(double tickMs, double renderMs, double swapMs);
    uint64_t nowNs() const { return clockOverride_ ? clockOverride_() : MonotonicNs(); }

    struct StageWindowSnapshot {
        uint32_t count = 0;
        double tickP50Ms = 0.0;
        double tickP99Ms = 0.0;
        double tickMaxMs = 0.0;
        double renderP50Ms = 0.0;
        double renderP99Ms = 0.0;
        double renderMaxMs = 0.0;
        double swapP50Ms = 0.0;
        double swapP99Ms = 0.0;
        double swapMaxMs = 0.0;
    };
    StageWindowSnapshot stageWindow() const;

    TelemetryQueue queue_;
    FrameSampleRing ring_;
    AnomalyDetector detector_;
    TelemetryEncoder encoder_;
    std::unique_ptr<OutputSink> sink_;
    uint64_t (*clockOverride_)() = nullptr;

    std::thread worker_;
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> workerExited_{false};
    std::atomic<bool> shutdownCalled_{false};
    std::atomic<bool> gpuLateWarned_{false};
    std::atomic<bool> rendererRecorded_{false};
    std::atomic<uint64_t> sequence_{0}; // worker thread only
    std::atomic<uint64_t> linesEmitted_{0};

    // Render-thread-only state.
    uint64_t nextFrameIndex_ = 1;
    uint64_t lastSubmitNs_ = 0;
    uint64_t lastSnapshotNs_ = 0;
    uint64_t lastAggregateNs_ = 0;
    uint64_t lastRingBusyEventNs_ = 0;
    uint64_t coalescedRingBusyDrops_ = 0;
    double tickSamples_[kIntervalWindowSize] = {};
    double renderSamples_[kIntervalWindowSize] = {};
    double swapSamples_[kIntervalWindowSize] = {};
    size_t stageSampleCount_ = 0;
    size_t stageSampleCursor_ = 0;
    // Set when a burst opens so future frames in [T, T+299] are streamed;
    // ordinary frames outside anomaly/burst ranges stay in the runtime ring.
    std::atomic<uint64_t> activeBurstStartFrame_{0};
    std::atomic<uint64_t> activeBurstEndFrame_{0};
};

} // namespace yoghourt_telemetry
