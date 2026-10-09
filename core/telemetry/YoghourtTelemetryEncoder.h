#pragma once

#include "YoghourtTelemetryRecords.h"

#include <string>

namespace yoghourt_telemetry {

// Reserved stdout prefix the host's launcher routes on. Every record is one
// UTF-8 JSON line: prefix + envelope + kind payload + '\n'. No NaN is ever
// emitted; unavailable numeric fields are omitted (the host CSV leaves them
// blank).
extern const char kTelemetryLinePrefix[];

class TelemetryEncoder {
public:
    // sessionID comes from YOGHOURT_SESSION_ID and is JSON-escaped on emit.
    // The string is copied: the collector must not depend on the env pointer.
    explicit TelemetryEncoder(const char *sessionID);

    // sequence is assigned by the worker at dequeue time; the stream is
    // therefore strictly monotonic per session.
    std::string encode(uint64_t sequence, const TelemetryRecord &record) const;

private:
    std::string sessionID_;
};

} // namespace yoghourt_telemetry
