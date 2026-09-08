#pragma once

#include <cstddef>
#include <cstdint>

namespace yoghourt_telemetry {

// Protocol-level limits and policy constants. These are fixed, not settings:
// they define the on-the-wire record shape and the anomaly contract that the
// host-side decoder and CSV schema are built against.
static constexpr size_t kFrameRingCapacity = 3600; // ~60 s of history at 60 fps
static constexpr size_t kIntervalWindowSize = 120; // anomaly baseline window
static constexpr size_t kWarmupIntervals = 120;    // valid intervals before anomalies are raised
static constexpr double kAnomalyIntervalFactor = 1.5;
static constexpr size_t kBurstConsecutiveAnomalies = 5;
static constexpr size_t kBurstRearmNormalIntervals = 30;
static constexpr uint64_t kBurstPrehistoryFrames = 300;        // [T-300, T-1] + pending [T+1, T+299]
static constexpr uint64_t kBurstPendingFutureFrames = 299;
static constexpr double kDiscontinuityIntervalMs = 1000.0;     // pause/rebuild/occlusion boundary
static constexpr size_t kQueueCapacity = 1024;                 // must stay a power of two
static constexpr uint64_t kSnapshotIntervalMs = 250;
static constexpr uint64_t kAggregateIntervalMs = 1000;
static constexpr uint64_t kRingBusyDropCoalesceMs = 250;       // sustained saturation would flood the channel

enum class RecordKind : uint8_t {
    frame = 0,
    snapshot = 1,
    aggregate = 2,
    event = 3,
    burst = 4,
    lifecycle = 5,
};

// Burst dumps, events, and lifecycle records outrank HUD snapshots and frame
// samples when the queue must evict.
inline bool IsHighPriority(RecordKind kind) {
    return kind == RecordKind::event || kind == RecordKind::burst || kind == RecordKind::lifecycle;
}

// Frame sample reason: 0 = ordinary, 1 = anomaly, 2 = burst range.
static constexpr uint8_t kFrameReasonNone = 0;
static constexpr uint8_t kFrameReasonAnomaly = 1;
static constexpr uint8_t kFrameReasonBurst = 2;

struct FrameSample {
    uint64_t frameIndex = 0;
    uint64_t monotonicNs = 0;
    double engineIntervalMs = 0.0;
    bool hasInterval = false;  // false for the first frame and across discontinuities
    double gpuFrameMs = -1.0;  // -1 until completion backfill lands
    bool presented = false;
    uint64_t telemetryDropped = 0;
    uint64_t criticalTelemetryDropped = 0;
    uint8_t reason = kFrameReasonNone;
};

// Small value record: producers only copy this struct, never format JSON,
// write files, or wait on the pipe.
struct TelemetryRecord {
    RecordKind kind = RecordKind::frame;
    uint64_t monotonicNs = 0;

    FrameSample frame; // frame payload

    // snapshot/aggregate payload
    struct Stats {
        double fps = 0.0;
        uint32_t intervalCount = 0;
        double intervalP50Ms = 0.0;
        double intervalP99Ms = 0.0;
        double intervalMaxMs = 0.0;
        uint32_t gpuCount = 0;
        double gpuP50Ms = 0.0;
        double gpuP99Ms = 0.0;
        double gpuMaxMs = 0.0;
        double lastGpuFrameMs = -1.0;
        uint64_t gpuTimingLateDrop = 0;
        uint64_t telemetryDropped = 0;
        uint64_t criticalTelemetryDropped = 0;
    } stats;

    // event/burst/lifecycle payload
    char eventType[28] = {};
    char detail[80] = {};
    uint64_t startFrameIndex = 0;
    uint64_t endFrameIndex = 0;
    uint64_t triggerFrameIndex = 0; // burst trigger T
    uint64_t targetFrameCount = 0;  // requested range, including unavailable history
    uint64_t aux = 0; // e.g. unavailablePrehistory, coalesced event count, lastFrameIndex
};

} // namespace yoghourt_telemetry
