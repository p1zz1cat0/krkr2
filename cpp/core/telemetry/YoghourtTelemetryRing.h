#pragma once

#include "YoghourtTelemetryRecords.h"

#include <atomic>
#include <stddef.h>

namespace yoghourt_telemetry {

// Fixed-capacity ring of the most recent FrameSamples. The render thread
// writes one slot per submission; Metal completion threads backfill the GPU
// duration by frameIndex. Every accessor takes the per-slot spinlock and
// revalidates the slot tag, so a completion that arrives after its slot was
// reused is counted as a late drop (gpuTimingLateDrop) and never lands on
// another frame. A non-zero late-drop count therefore indicates GPU stall,
// queue overrun, or an ownership bug — not ordinary noise (3600 slots cover
// ~60 s at 60 fps).
class FrameSampleRing {
public:
    FrameSampleRing();

    FrameSampleRing(const FrameSampleRing &) = delete;
    FrameSampleRing &operator=(const FrameSampleRing &) = delete;

    // Render thread: publishes a new sample, overwriting the oldest slot.
    void write(const FrameSample &sample);

    // Render thread: annotates the reason of a sample it just wrote.
    void setReason(uint64_t frameIndex, uint8_t reason);

    // Completion thread: backfills the GPU duration and records it in the
    // GPU stats window. On success, `out` receives the post-backfill sample
    // (captured under the slot lock, so the frame record streams a consistent
    // view). Returns false when the slot no longer holds frameIndex (late
    // completion); the late-drop counter is incremented, the GPU window is
    // left untouched, and no sample is produced.
    bool completeGpu(uint64_t frameIndex, double gpuFrameMs, bool succeeded, FrameSample &out);

    // Render thread: backfills the three CPU/wait stages measured around
    // cocos Director::drawScene. Returns false if this frame was not sampled.
    bool completeStages(uint64_t frameIndex, double tickMs, double renderMs, double swapMs);

    // Present thread: marks the drawable as presented; a late arrival is
    // ignored (the flag is best-effort, the ring keeps the final truth).
    void markPresented(uint64_t frameIndex);

    // Render thread (burst prehistory): copies the sample when still present,
    // regardless of streaming state.
    bool read(uint64_t frameIndex, FrameSample &out) const;

    // Completion thread (lagged frame streaming): claims the PREVIOUS frame's
    // final sample for emission. Streaming with one frame of lag lets the
    // presented flag (which fires at vsync, after GPU completion) settle, so
    // emitted frame rows carry the ring's near-final state. Returns false
    // when the slot is missing, not completed yet, or already streamed. The
    // shutdown-only allowIncomplete path emits a still-owned sample with an
    // unavailable GPU value so a final anomaly is not lost on GPU hang.
    bool takeForStreaming(uint64_t frameIndex, FrameSample &out, bool allowIncomplete = false);

    // Render thread (burst prehistory): claims any un-streamed sample for
    // re-emission with a burst reason. Frames already streamed earlier are
    // re-read by the caller via read() when a fresh burst-marked copy is
    // required (the host deduplicates by frameIndex).
    bool takeForBurst(uint64_t frameIndex, FrameSample &out);

    struct GpuWindowSnapshot {
        uint32_t count = 0;
        double p50Ms = 0.0;
        double p99Ms = 0.0;
        double maxMs = 0.0;
        double lastMs = -1.0;
    };

    GpuWindowSnapshot gpuWindow() const;
    uint64_t gpuTimingLateDrop() const { return gpuTimingLateDrop_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        mutable std::atomic_flag guard = ATOMIC_FLAG_INIT;
        std::atomic<uint64_t> tag{0}; // frameIndex while the slot holds it, 0 during (re)init
        FrameSample sample;
        bool completed = false; // GPU completion applied
        bool streamed = false;  // already claimed for emission
    };

    Slot &slotFor(uint64_t frameIndex) { return slots_[frameIndex % kFrameRingCapacity]; }
    const Slot &slotFor(uint64_t frameIndex) const { return slots_[frameIndex % kFrameRingCapacity]; }

    void pushGpuSampleLocked(double ms);

    Slot slots_[kFrameRingCapacity];

    // GPU stats window: written on completion threads, sampled by the render
    // thread for snapshots/aggregates.
    mutable std::atomic_flag gpuGuard_ = ATOMIC_FLAG_INIT;
    double gpuSamples_[kIntervalWindowSize] = {};
    size_t gpuSampleCount_ = 0;
    size_t gpuSampleCursor_ = 0;
    double gpuLastMs_ = -1.0;

    std::atomic<uint64_t> gpuTimingLateDrop_{0};
};

} // namespace yoghourt_telemetry
