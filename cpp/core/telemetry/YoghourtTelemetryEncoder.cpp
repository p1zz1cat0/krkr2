#include "YoghourtTelemetryEncoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace yoghourt_telemetry {

const char kTelemetryLinePrefix[] = "[YOGHOURT_TELEMETRY_V1]";

namespace {

void appendEscaped(std::string &out, const char *value) {
    out.push_back('"');
    for (const char *p = value ? value : ""; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", c);
                out += buffer;
            } else {
                out.push_back(*p);
            }
            break;
        }
    }
    out.push_back('"');
}

void appendKey(std::string &out, const char *key) {
    if (out.size() > 1) out.push_back(',');
    appendEscaped(out, key);
    out.push_back(':');
}

void appendUint(std::string &out, const char *key, uint64_t value) {
    appendKey(out, key);
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    out += buffer;
}

void appendDouble(std::string &out, const char *key, double value, const char *format) {
    if (!std::isfinite(value)) return; // unavailable fields are omitted, never NaN/Inf
    appendKey(out, key);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), format, value);
    out += buffer;
}

void appendBool(std::string &out, const char *key, bool value) {
    appendKey(out, key);
    out += value ? "true" : "false";
}

void appendStringIfNotEmpty(std::string &out, const char *key, const char *value) {
    if (!value || !value[0]) return;
    appendKey(out, key);
    appendEscaped(out, value);
}

const char *reasonName(uint8_t reason) {
    switch (reason) {
    case kFrameReasonAnomaly: return "anomaly";
    case kFrameReasonBurst: return "burst";
    default: return nullptr;
    }
}

} // namespace

TelemetryEncoder::TelemetryEncoder(const char *sessionID) : sessionID_(sessionID ? sessionID : "") {}

std::string TelemetryEncoder::encode(uint64_t sequence, const TelemetryRecord &record) const {
    std::string out;
    out.reserve(224);
    out += kTelemetryLinePrefix;
    out += "{\"version\":1";
    appendKey(out, "sessionID");
    appendEscaped(out, sessionID_.c_str());
    appendUint(out, "sequence", sequence);
    appendUint(out, "monotonicNs", record.monotonicNs);

    switch (record.kind) {
    case RecordKind::frame: {
        appendKey(out, "kind");
        out += "\"frame\"";
        appendUint(out, "frameIndex", record.frame.frameIndex);
        if (record.frame.hasInterval) {
            appendDouble(out, "engineIntervalMs", record.frame.engineIntervalMs, "%.3f");
        }
        if (record.frame.gpuFrameMs >= 0.0) {
            appendDouble(out, "gpuFrameMs", record.frame.gpuFrameMs, "%.3f");
        }
        appendBool(out, "presented", record.frame.presented);
        appendUint(out, "telemetryDropped", record.frame.telemetryDropped);
        appendUint(out, "criticalTelemetryDropped", record.frame.criticalTelemetryDropped);
        if (const char *reason = reasonName(record.frame.reason)) {
            appendKey(out, "reason");
            appendEscaped(out, reason);
        }
        break;
    }
    case RecordKind::snapshot:
    case RecordKind::aggregate: {
        appendKey(out, "kind");
        out += (record.kind == RecordKind::snapshot) ? "\"snapshot\"" : "\"aggregate\"";
        appendDouble(out, "fps", record.stats.fps, "%.2f");
        appendUint(out, "intervalCount", record.stats.intervalCount);
        appendDouble(out, "intervalP50Ms", record.stats.intervalP50Ms, "%.3f");
        appendDouble(out, "intervalP99Ms", record.stats.intervalP99Ms, "%.3f");
        appendDouble(out, "intervalMaxMs", record.stats.intervalMaxMs, "%.3f");
        appendUint(out, "gpuCount", record.stats.gpuCount);
        appendDouble(out, "gpuP50Ms", record.stats.gpuP50Ms, "%.3f");
        appendDouble(out, "gpuP99Ms", record.stats.gpuP99Ms, "%.3f");
        appendDouble(out, "gpuMaxMs", record.stats.gpuMaxMs, "%.3f");
        if (record.stats.lastGpuFrameMs >= 0.0) {
            appendDouble(out, "lastGpuFrameMs", record.stats.lastGpuFrameMs, "%.3f");
        }
        appendUint(out, "gpuTimingLateDrop", record.stats.gpuTimingLateDrop);
        appendUint(out, "telemetryDropped", record.stats.telemetryDropped);
        appendUint(out, "criticalTelemetryDropped", record.stats.criticalTelemetryDropped);
        break;
    }
    case RecordKind::event: {
        appendKey(out, "kind");
        out += "\"event\"";
        appendKey(out, "eventType");
        appendEscaped(out, record.eventType);
        appendStringIfNotEmpty(out, "detail", record.detail);
        if (record.startFrameIndex > 0) appendUint(out, "startFrameIndex", record.startFrameIndex);
        if (record.endFrameIndex > 0) appendUint(out, "endFrameIndex", record.endFrameIndex);
        if (record.aux > 0) appendUint(out, "count", record.aux);
        break;
    }
    case RecordKind::burst: {
        appendKey(out, "kind");
        out += "\"burst\"";
        appendUint(out, "startFrameIndex", record.startFrameIndex);
        appendUint(out, "endFrameIndex", record.endFrameIndex);
        appendUint(out, "triggerFrameIndex", record.triggerFrameIndex);
        appendUint(out, "targetFrameCount", record.targetFrameCount);
        appendUint(out, "unavailablePrehistory", record.aux);
        break;
    }
    case RecordKind::lifecycle: {
        appendKey(out, "kind");
        out += "\"lifecycle\"";
        appendKey(out, "event");
        appendEscaped(out, record.eventType);
        appendStringIfNotEmpty(out, "detail", record.detail);
        if (record.aux > 0) appendUint(out, "lastFrameIndex", record.aux);
        break;
    }
    }

    out += "}\n";
    return out;
}

} // namespace yoghourt_telemetry
