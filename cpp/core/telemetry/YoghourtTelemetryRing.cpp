#include "YoghourtTelemetryRing.h"

#include <algorithm>
#include <cstring>

namespace yoghourt_telemetry {

namespace {

// Slot sections are a handful of stores; a plain spinlock keeps the
// render/completion interleave deterministic without touching the hot path
// beyond tens of nanoseconds per frame.
struct SlotLock {
    explicit SlotLock(std::atomic_flag &flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~SlotLock() { flag_.clear(std::memory_order_release); }

    SlotLock(const SlotLock &) = delete;
    SlotLock &operator=(const SlotLock &) = delete;

private:
    std::atomic_flag &flag_;
};

double percentileOf(double *sorted, size_t count, double fraction) {
    if (count == 0) return 0.0;
    const size_t index = static_cast<size_t>(static_cast<double>(count - 1) * fraction);
    return sorted[index];
}

} // namespace

FrameSampleRing::FrameSampleRing() = default;

void FrameSampleRing::write(const FrameSample &sample) {
    Slot &slot = slotFor(sample.frameIndex);
    SlotLock lock(slot.guard);
    slot.tag.store(0, std::memory_order_relaxed);
    slot.sample = sample;
    slot.completed = false;
    slot.streamed = false;
    slot.tag.store(sample.frameIndex, std::memory_order_release);
}

void FrameSampleRing::setReason(uint64_t frameIndex, uint8_t reason) {
    Slot &slot = slotFor(frameIndex);
    SlotLock lock(slot.guard);
    if (slot.tag.load(std::memory_order_acquire) != frameIndex) return;
    slot.sample.reason = reason;
}

bool FrameSampleRing::completeGpu(uint64_t frameIndex, double gpuFrameMs, bool succeeded, FrameSample &out) {
    Slot &slot = slotFor(frameIndex);
    {
        SlotLock lock(slot.guard);
        if (slot.tag.load(std::memory_order_acquire) != frameIndex) {
            gpuTimingLateDrop_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (succeeded && gpuFrameMs > 0.0 && slot.sample.gpuFrameMs < 0.0) {
            slot.sample.gpuFrameMs = gpuFrameMs;
        }
        slot.completed = true;
        out = slot.sample;
    }
    if (succeeded && gpuFrameMs > 0.0) {
        SlotLock gpuLock(gpuGuard_);
        pushGpuSampleLocked(gpuFrameMs);
    }
    return true;
}

void FrameSampleRing::markPresented(uint64_t frameIndex) {
    Slot &slot = slotFor(frameIndex);
    SlotLock lock(slot.guard);
    if (slot.tag.load(std::memory_order_acquire) != frameIndex) return;
    slot.sample.presented = true;
}

bool FrameSampleRing::read(uint64_t frameIndex, FrameSample &out) const {
    const Slot &slot = slotFor(frameIndex);
    SlotLock lock(slot.guard);
    if (slot.tag.load(std::memory_order_acquire) != frameIndex) return false;
    out = slot.sample;
    return true;
}

bool FrameSampleRing::takeForStreaming(uint64_t frameIndex, FrameSample &out, bool allowIncomplete) {
    Slot &slot = slotFor(frameIndex);
    SlotLock lock(slot.guard);
    if (slot.tag.load(std::memory_order_acquire) != frameIndex) return false;
    if ((!slot.completed && !allowIncomplete) || slot.streamed) return false;
    slot.streamed = true;
    out = slot.sample;
    return true;
}

bool FrameSampleRing::takeForBurst(uint64_t frameIndex, FrameSample &out) {
    Slot &slot = slotFor(frameIndex);
    SlotLock lock(slot.guard);
    if (slot.tag.load(std::memory_order_acquire) != frameIndex) return false;
    // Do not consume an in-flight frame here. Its completion callback must
    // still be allowed to backfill GPU time and stream the final sample while
    // the burst is open.
    if (!slot.completed) return false;
    slot.streamed = true;
    out = slot.sample;
    return true;
}

void FrameSampleRing::pushGpuSampleLocked(double ms) {
    gpuSamples_[gpuSampleCursor_] = ms;
    gpuSampleCursor_ = (gpuSampleCursor_ + 1) % kIntervalWindowSize;
    gpuSampleCount_ = std::min(gpuSampleCount_ + 1, kIntervalWindowSize);
    gpuLastMs_ = ms;
}

FrameSampleRing::GpuWindowSnapshot FrameSampleRing::gpuWindow() const {
    double sorted[kIntervalWindowSize];
    size_t count = 0;
    double last = -1.0;
    {
        SlotLock lock(gpuGuard_);
        count = gpuSampleCount_;
        last = gpuLastMs_;
        std::memcpy(sorted, gpuSamples_, sizeof(double) * count);
    }
    std::sort(sorted, sorted + count);
    GpuWindowSnapshot snapshot;
    snapshot.count = static_cast<uint32_t>(count);
    snapshot.p50Ms = percentileOf(sorted, count, 0.50);
    snapshot.p99Ms = percentileOf(sorted, count, 0.99);
    snapshot.maxMs = count > 0 ? sorted[count - 1] : 0.0;
    snapshot.lastMs = last;
    return snapshot;
}

} // namespace yoghourt_telemetry
