#include "YoghourtTelemetryQueue.h"

namespace yoghourt_telemetry {

TelemetryQueue::TelemetryQueue(size_t capacity)
    : cells_(new Cell[capacity]), capacity_(capacity), capacityMask_(capacity - 1) {
    // kQueueCapacity and every test capacity stay a power of two so the mask
    // below maps positions onto cells exactly once per wrap.
    for (size_t i = 0; i < capacity; ++i) {
        cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
}

TelemetryQueue::~TelemetryQueue() = default;

bool TelemetryQueue::pushRaw(const TelemetryRecord &record) {
    Cell *cell;
    size_t pos = enqueuePos_.load(std::memory_order_relaxed);
    for (;;) {
        cell = &cells_[pos & capacityMask_];
        const size_t seq = cell->sequence.load(std::memory_order_acquire);
        const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
        if (dif == 0) {
            if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; // full
        } else {
            pos = enqueuePos_.load(std::memory_order_relaxed);
        }
    }
    cell->record = record;
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
}

bool TelemetryQueue::popRaw(TelemetryRecord &out) {
    Cell *cell;
    size_t pos = dequeuePos_.load(std::memory_order_relaxed);
    for (;;) {
        cell = &cells_[pos & capacityMask_];
        const size_t seq = cell->sequence.load(std::memory_order_acquire);
        const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        if (dif == 0) {
            if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; // empty
        } else {
            pos = dequeuePos_.load(std::memory_order_relaxed);
        }
    }
    out = cell->record;
    cell->sequence.store(pos + capacity_, std::memory_order_release);
    return true;
}

void TelemetryQueue::countDropFor(const TelemetryRecord &record) {
    if (IsHighPriority(record.kind)) {
        criticalDropped_.fetch_add(1, std::memory_order_relaxed);
    } else {
        normalDropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool TelemetryQueue::push(const TelemetryRecord &record) {
    if (pushRaw(record)) {
        return true;
    }

    // Full: decide by priority. Normal records are dropped; high-priority
    // records may evict the oldest entry unless that entry is itself
    // high-priority (critical records never evict each other).
    if (!IsHighPriority(record.kind)) {
        countDropFor(record);
        return false;
    }

    TelemetryRecord evicted;
    if (!popRaw(evicted)) {
        // Raced with a concurrent consumer draining the slot we needed; the
        // retry below may still succeed.
        if (pushRaw(record)) {
            return true;
        }
        countDropFor(record);
        return false;
    }
    if (IsHighPriority(evicted.kind)) {
        // Put the older critical record back; this incoming record becomes
        // the critical drop. Re-push can only fail under a full-queue race,
        // in which case both records are counted and nothing is lost
        // silently.
        if (!pushRaw(evicted)) {
            countDropFor(evicted);
        }
        countDropFor(record);
        return false;
    }
    normalDropped_.fetch_add(1, std::memory_order_relaxed);
    if (pushRaw(record)) {
        return true;
    }
    // Raced: another producer took the freed slot. The incoming high-priority
    // record is the drop; the evicted normal record was already counted.
    countDropFor(record);
    return false;
}

bool TelemetryQueue::pop(TelemetryRecord &out) {
    return popRaw(out);
}

} // namespace yoghourt_telemetry
