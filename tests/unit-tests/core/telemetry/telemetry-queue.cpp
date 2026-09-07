#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "YoghourtTelemetryQueue.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace yoghourt_telemetry;

namespace {

TelemetryRecord makeRecord(RecordKind kind, uint64_t marker) {
    TelemetryRecord record;
    record.kind = kind;
    record.aux = marker;
    return record;
}

} // namespace

TEST_CASE("queue preserves order for a single producer and consumer", "[telemetry][queue]") {
    TelemetryQueue queue(128);
    for (uint64_t i = 1; i <= 100; ++i) {
        REQUIRE(queue.push(makeRecord(RecordKind::frame, i)));
    }
    for (uint64_t i = 1; i <= 100; ++i) {
        TelemetryRecord out;
        REQUIRE(queue.pop(out));
        REQUIRE(out.aux == i);
    }
    TelemetryRecord out;
    REQUIRE_FALSE(queue.pop(out));
}

TEST_CASE("queue is non-blocking and counts drops for normal records when full", "[telemetry][queue]") {
    TelemetryQueue queue(16);
    for (uint64_t i = 0; i < 16; ++i) {
        REQUIRE(queue.push(makeRecord(RecordKind::frame, i)));
    }
    // Normal record into a full queue: dropped, oldest kept.
    REQUIRE_FALSE(queue.push(makeRecord(RecordKind::frame, 999)));
    REQUIRE(queue.normalDropped() == 1);
    REQUIRE(queue.criticalDropped() == 0);

    TelemetryRecord out;
    REQUIRE(queue.pop(out));
    REQUIRE(out.aux == 0); // oldest survives; the dropped record was the newest
}

TEST_CASE("high-priority records evict the oldest normal record when full", "[telemetry][queue]") {
    TelemetryQueue queue(16);
    for (uint64_t i = 0; i < 16; ++i) {
        REQUIRE(queue.push(makeRecord(RecordKind::frame, i)));
    }
    REQUIRE(queue.push(makeRecord(RecordKind::lifecycle, 42)));
    REQUIRE(queue.normalDropped() == 1);
    REQUIRE(queue.criticalDropped() == 0);

    // The evicted oldest record is gone; the high-priority record re-enters
    // at the tail, so the surviving order is [1..15] then 42.
    for (uint64_t i = 1; i <= 15; ++i) {
        TelemetryRecord out;
        REQUIRE(queue.pop(out));
        REQUIRE(out.aux == i);
    }
    TelemetryRecord out;
    REQUIRE(queue.pop(out));
    REQUIRE(out.aux == 42);
    REQUIRE_FALSE(queue.pop(out));
}

TEST_CASE("critical records are counted, never evicted by each other", "[telemetry][queue]") {
    TelemetryQueue queue(4);
    for (uint64_t i = 0; i < 4; ++i) {
        REQUIRE(queue.push(makeRecord(RecordKind::lifecycle, i)));
    }
    REQUIRE_FALSE(queue.push(makeRecord(RecordKind::burst, 100)));
    REQUIRE(queue.criticalDropped() == 1);
    REQUIRE(queue.normalDropped() == 0);

    // The failed push temporarily evicted the oldest critical record and put
    // it back at the tail; all four must survive, in a rotated order.
    std::set<uint64_t> seen;
    for (int i = 0; i < 4; ++i) {
        TelemetryRecord out;
        REQUIRE(queue.pop(out));
        seen.insert(out.aux);
    }
    REQUIRE(seen == std::set<uint64_t>{0, 1, 2, 3});
}

TEST_CASE("multi-producer push drains without deadlock and preserves counts", "[telemetry][queue]") {
    TelemetryQueue queue(1024);
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000;
    std::atomic<uint64_t> accepted{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, &accepted, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                if (queue.push(makeRecord(RecordKind::frame, static_cast<uint64_t>(p * kPerProducer + i)))) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &thread : producers) {
        thread.join();
    }

    // Capacity 1024: the rest were dropped, all accounted for.
    REQUIRE(accepted.load() == 1024);
    REQUIRE(queue.normalDropped() == kProducers * kPerProducer - 1024);
    REQUIRE(queue.criticalDropped() == 0);

    size_t drained = 0;
    TelemetryRecord out;
    while (queue.pop(out)) {
        ++drained;
    }
    REQUIRE(drained == 1024);
}
