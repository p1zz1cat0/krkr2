#pragma once

#include "YoghourtTelemetryRecords.h"

#include <atomic>
#include <memory>
#include <stddef.h>

namespace yoghourt_telemetry {

// Bounded MPMC queue (Vyukov bounded queue cells) with priority-aware
// eviction. Producers (render thread, Metal completion/present queues) never
// block and never format output. When the queue is full:
//   - a normal record is dropped and counted as normalDropped (telemetryDropped);
//   - a high-priority record evicts the oldest entry: normal entries are
//     counted as normalDropped, another high-priority entry is put back and
//     the incoming record is counted as criticalDropped.
// Sequence numbers are not stored here; the single worker assigns them at
// dequeue, so the emitted stream is gap-free by construction and drops are
// visible through the counters carried by later records.
class TelemetryQueue {
public:
    explicit TelemetryQueue(size_t capacity = kQueueCapacity);
    ~TelemetryQueue();

    TelemetryQueue(const TelemetryQueue &) = delete;
    TelemetryQueue &operator=(const TelemetryQueue &) = delete;

    // Producer side, any thread, non-blocking. Returns false when the record
    // was dropped (the matching drop counter has been incremented).
    bool push(const TelemetryRecord &record);

    // Consumer side; multiple consumers are allowed (eviction uses this too).
    bool pop(TelemetryRecord &out);

    uint64_t normalDropped() const { return normalDropped_.load(std::memory_order_relaxed); }
    uint64_t criticalDropped() const { return criticalDropped_.load(std::memory_order_relaxed); }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        TelemetryRecord record;
    };

    bool pushRaw(const TelemetryRecord &record);
    bool popRaw(TelemetryRecord &out);
    void countDropFor(const TelemetryRecord &record);

    std::unique_ptr<Cell[]> cells_;
    const size_t capacity_;
    const size_t capacityMask_;
    std::atomic<size_t> enqueuePos_{0};
    std::atomic<size_t> dequeuePos_{0};
    std::atomic<uint64_t> normalDropped_{0};
    std::atomic<uint64_t> criticalDropped_{0};
};

} // namespace yoghourt_telemetry
