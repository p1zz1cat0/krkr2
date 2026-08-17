#include "ExtransMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace extrans {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::int32_t kFixedOne = 65536;

std::int32_t TruncateToInt(double value) {
    if(!std::isfinite(value) ||
       value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
       value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        throw std::overflow_error("extrans integer conversion overflow");
    return static_cast<std::int32_t>(value); // C++ and Perl int truncate to zero
}

TurnTable GenerateTurnTable() {
    TurnTable table{};
    for(std::int32_t phase = 1; phase <= 62; ++phase) {
        std::int32_t p;
        std::int32_t d;
        std::int32_t ax;
        std::int32_t ay;
        if(phase < 32) {
            p = TruncateToInt(static_cast<double>(phase * phase) / 31.0);
            d = TruncateToInt(std::sin(p * kPi / 64.0) * 4.0);
            ax = p - d;
            ay = 63 - p - d;
        } else {
            const std::int32_t inverse = 63 - phase;
            p = 63 - TruncateToInt(static_cast<double>(inverse * inverse) / 31.0);
            d = TruncateToInt(std::sin((63 - p) * kPi / 64.0) * 4.0);
            ax = 63 - p + d;
            ay = p + d;
        }
        const std::int32_t bx = 63 - ax;
        const std::int32_t by = 63 - ay;

        for(std::int32_t y = 0; y < 64; ++y) {
            std::int32_t left;
            if(y <= ay) {
                left = ay == 0 ? 0 : TruncateToInt(static_cast<double>(ax * y) / ay);
            } else {
                left = ay == 63
                    ? 63
                    : ax + TruncateToInt(
                          static_cast<double>((63 - ax) * (y - ay)) /
                          (63 - ay));
            }

            std::int32_t right;
            if(y <= by) {
                right = by == 0
                    ? 63
                    : TruncateToInt(static_cast<double>(bx * y) / by);
            } else {
                right = by == 63
                    ? 63
                    : bx + TruncateToInt(
                          static_cast<double>((63 - bx) * (y - by)) /
                          (63 - by));
            }
            if(left > right)
                right = left;

            const std::int32_t length = right - left + 1;
            std::int32_t sx;
            std::int32_t sy;
            if(y <= ay) {
                sx = 0;
                sy = ay ? TruncateToInt(static_cast<double>(63 * y) / ay) : 0;
            } else {
                sx = TruncateToInt(static_cast<double>(63 * (y - ay)) /
                                   (63 - ay));
                sy = 63;
            }

            std::int32_t ex;
            std::int32_t ey;
            if(y <= by) {
                ex = by ? TruncateToInt(static_cast<double>(63 * y) / by) : 63;
                ey = 0;
            } else {
                ex = 63;
                ey = TruncateToInt(static_cast<double>(63 * (y - by)) /
                                   (63 - by));
            }

            TurnParameters &entry = table[phase][y];
            entry.start = left;
            entry.length = length;
            entry.sourceX = sx * kFixedOne;
            entry.sourceY = sy * kFixedOne;
            if(length >= 2) {
                entry.stepX = TruncateToInt(
                    static_cast<double>(ex - sx) / (length - 1) * kFixedOne);
                entry.stepY = TruncateToInt(
                    static_cast<double>(ey - sy) / (length - 1) * kFixedOne);
            }
        }
    }
    return table;
}

std::int32_t RoundFixed(double value) {
    return TruncateToInt(value < 0.0 ? value - 0.5 : value + 0.5);
}

} // namespace

bool ValidateWaveParameters(std::int32_t width, std::int32_t maxHeight,
                            double maxOmega) {
    if(width <= 0 || !std::isfinite(maxOmega))
        return false;
    const std::int64_t magnitude = maxHeight < 0
        ? -static_cast<std::int64_t>(maxHeight)
        : static_cast<std::int64_t>(maxHeight);
    return magnitude <=
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) -
            width;
}

WaveFrame ComputeWaveFrame(std::uint64_t currentTime, std::uint64_t duration,
                           std::int32_t imageHeight,
                           std::int32_t maxHeight, double maxOmega,
                           std::int32_t waveType) {
    if(duration < 2 || imageHeight <= 0 ||
       !ValidateWaveParameters(1, maxHeight, maxOmega) ||
       std::fabs(maxOmega) >
           std::numeric_limits<double>::max() / imageHeight)
        throw std::invalid_argument("invalid wave parameters");
    currentTime = std::min(currentTime, duration);
    const std::uint64_t halfTime = duration / 2;
    std::uint64_t folded = currentTime;
    if(folded >= halfTime)
        folded = duration - folded;
    const double amplitude = std::sin(
        (kPi / 2.0) * static_cast<double>(folded) /
        static_cast<double>(halfTime));

    WaveFrame frame;
    // The official member is tjs_int: truncate once before the scanline sine.
    frame.height = TruncateToInt(amplitude * maxHeight);
    switch(waveType) {
        case 1:
            frame.omega = maxOmega *
                static_cast<double>(duration - currentTime) /
                static_cast<double>(duration);
            break;
        case 2:
            frame.omega = maxOmega * static_cast<double>(currentTime) /
                static_cast<double>(duration);
            break;
        default:
            frame.omega = maxOmega * amplitude;
            break;
    }
    frame.radianStart = -frame.omega * (imageHeight / 2);
    frame.blendRatio = static_cast<std::int32_t>(
        static_cast<unsigned __int128>(currentTime) * 255 / duration);
    return frame;
}

bool ValidateMosaicParameters(std::int32_t width, std::int32_t height,
                              std::int64_t maxBlockSize) {
    // 2^20 is far beyond practical layer dimensions while remaining exactly
    // representable in the ES highp coordinate shader.
    return width > 0 && height > 0 && maxBlockSize >= 2 &&
        maxBlockSize <= (1 << 20);
}

MosaicFrame ComputeMosaicFrame(std::uint64_t currentTime,
                               std::uint64_t duration, std::int32_t width,
                               std::int32_t height,
                               std::int32_t maxBlockSize) {
    if(duration < 2 ||
       !ValidateMosaicParameters(width, height, maxBlockSize))
        throw std::invalid_argument("invalid mosaic parameters");
    currentTime = std::min(currentTime, duration);
    const std::uint64_t halfTime = duration / 2;
    std::uint64_t folded = currentTime;
    if(folded >= halfTime)
        folded = duration - folded;

    MosaicFrame frame;
    const auto scaled = static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(maxBlockSize - 2) * folded /
        halfTime);
    frame.blockSize = static_cast<std::int32_t>(scaled) + 2;
    const auto computeOffset = [block = frame.blockSize](std::int32_t size) {
        std::int32_t centerBlock = size / 2;
        centerBlock /= block;
        centerBlock *= block;
        std::int32_t offset = (size - block) / 2 - centerBlock;
        if(offset > 0)
            offset -= block;
        return offset;
    };
    frame.offsetX = computeOffset(width);
    frame.offsetY = computeOffset(height);
    frame.blendRatio = static_cast<std::int32_t>(
        static_cast<unsigned __int128>(currentTime) * 255 / duration);
    return frame;
}

std::int32_t ComputeMosaicSampleCoordinate(std::int32_t logicalCoordinate,
                                           std::int32_t imageSize,
                                           std::int32_t blockSize,
                                           std::int32_t offset) {
    if(logicalCoordinate < 0 || imageSize <= 0 || blockSize < 2)
        throw std::invalid_argument("invalid mosaic coordinate");
    const std::int64_t block =
        (static_cast<std::int64_t>(logicalCoordinate) - offset) / blockSize;
    const std::int64_t center = block * blockSize + offset + blockSize / 2;
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(center, 0, imageSize - 1));
}

bool IsFiniteRotateParameter(double value) {
    return std::isfinite(value);
}

std::int32_t TruncateRotateCoordinate(double value) {
    return TruncateToInt(value);
}

const TurnTable &GetTurnTable() {
    static const TurnTable table = GenerateTurnTable();
    return table;
}

const std::array<std::int32_t, kTurnPhaseCount> &GetTurnGloss() {
    static constexpr std::array<std::int32_t, kTurnPhaseCount> gloss = {
        0, 0, 0, 0, 16, 48, 80, 128, 192, 128, 80, 48, 16, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0,   0,   0,  0,  0, 0, 0, 0,
        0, 0, 0, 0, 0,  0,  0,  0,   0,   0,   0,  0,  0, 0, 0, 0,
        0, 0, 0, 0, 0,  0,  0,  0,   0,   0,   0,  0,  0, 0, 0, 0,
    };
    return gloss;
}

std::int32_t ComputeTurnGlobalPhase(std::uint64_t currentTime,
                                    std::uint64_t duration,
                                    std::int32_t width,
                                    std::int32_t height) {
    if(duration == 0 || width <= 0 || height <= 0)
        return 0;
    currentTime = std::min(currentTime, duration);
    const std::int64_t xCount = (static_cast<std::int64_t>(width) - 1) / 64 + 1;
    const std::int64_t yCount = (static_cast<std::int64_t>(height) - 1) / 64 + 1;
    const std::int64_t span = 64 + (xCount + yCount) * 2;
    const auto scaled = static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(currentTime) *
        static_cast<std::uint64_t>(span) / duration);
    const std::int64_t phase = static_cast<std::int64_t>(scaled) - yCount * 2;
    return static_cast<std::int32_t>(phase);
}

std::int32_t ComputeTurnTilePhase(std::int32_t globalPhase,
                                  std::int32_t tileX,
                                  std::int32_t tileY) {
    return std::clamp(globalPhase - (tileX - tileY) * 2, 0, 63);
}

TurnPixelMapping MapTurnPixel(std::int32_t globalPhase,
                              std::int32_t logicalX,
                              std::int32_t logicalY) {
    if(logicalX < 0 || logicalY < 0)
        return {true, false, 0, 0, 0};
    const std::int32_t tileX = logicalX / kTurnTileSize;
    const std::int32_t tileY = logicalY / kTurnTileSize;
    const std::int32_t withinX = logicalX % kTurnTileSize;
    const std::int32_t withinY = logicalY % kTurnTileSize;
    const std::int32_t phase =
        ComputeTurnTilePhase(globalPhase, tileX, tileY);
    if(phase == 0 || phase == 63)
        return {false, phase == 63, logicalX, logicalY, 0};

    const TurnParameters &entry = GetTurnTable()[phase][withinY];
    if(withinX < entry.start || withinX >= entry.start + entry.length)
        return {true, phase >= 32, 0, 0, GetTurnGloss()[phase]};
    const std::int32_t advance = withinX - entry.start;
    const std::int64_t fixedX = static_cast<std::int64_t>(entry.sourceX) +
                                static_cast<std::int64_t>(entry.stepX) * advance;
    const std::int64_t fixedY = static_cast<std::int64_t>(entry.sourceY) +
                                static_cast<std::int64_t>(entry.stepY) * advance;
    // The official implementation uses signed >> 16. Clang on the supported
    // macOS targets performs an arithmetic shift, equivalent to floor here.
    const auto floorFixed = [](std::int64_t value) -> std::int32_t {
        if(value >= 0)
            return static_cast<std::int32_t>(value / kFixedOne);
        return static_cast<std::int32_t>(
            -((-value + kFixedOne - 1) / kFixedOne));
    };
    return {false,
            phase >= 32,
            tileX * kTurnTileSize + floorFixed(fixedX),
            tileY * kTurnTileSize + floorFixed(fixedY),
            GetTurnGloss()[phase]};
}

std::uint32_t BlendARGB256(std::uint32_t source1, std::uint32_t source2,
                           std::int32_t ratio) {
    ratio = std::clamp(ratio, 0, 256);
    std::uint32_t result = 0;
    for(std::int32_t shift = 0; shift <= 24; shift += 8) {
        const std::int32_t first = (source1 >> shift) & 0xff;
        const std::int32_t second = (source2 >> shift) & 0xff;
        const std::int32_t value = first + ((second - first) * ratio >> 8);
        result |= static_cast<std::uint32_t>(value & 0xff) << shift;
    }
    return result;
}

std::uint32_t ApplyTurnGloss(std::uint32_t color, std::int32_t gloss) {
    const std::uint32_t whiteWithSourceAlpha =
        (color & 0xff000000u) | 0x00ffffffu;
    return BlendARGB256(color, whiteWithSourceAlpha, gloss);
}

bool ValidateRippleParameters(const RippleParameters &p) {
    if(p.width <= 0 || p.height <= 0 || p.centerX < 0 || p.centerY < 0 ||
       p.centerX >= p.width || p.centerY >= p.height)
        return false;
    if(p.rippleWidth != 16 && p.rippleWidth != 32 && p.rippleWidth != 64 &&
       p.rippleWidth != 128)
        return false;
    if(!std::isfinite(p.roundness) || p.roundness <= 0.0f ||
       !std::isfinite(p.speed))
        return false;
    if(p.maxDrift < 0 || p.maxDrift >= 128 || p.maxDrift >= p.width ||
       p.maxDrift >= p.height)
        return false;
    return true;
}

RippleTables GenerateRippleTables(const RippleParameters &p) {
    if(!ValidateRippleParameters(p))
        throw std::invalid_argument("invalid ripple parameters");

    RippleTables table;
    table.mapWidth = p.centerX < (p.width >> 1) ? p.width - p.centerX : p.centerX;
    table.mapHeight = p.centerY < (p.height >> 1) ? p.height - p.centerY : p.centerY;
    const std::size_t mapCount =
        static_cast<std::size_t>(table.mapWidth) * table.mapHeight;
    if(table.mapWidth <= 0 || table.mapHeight <= 0 ||
       mapCount > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t))
        throw std::overflow_error("ripple displacement table too large");
    table.displacement.resize(mapCount);

    const std::int32_t rippleMask = p.rippleWidth - 1;
    for(std::int32_t y = 0; y < table.mapHeight; ++y) {
        const float yy = (static_cast<float>(y) + 0.5f) * p.roundness;
        const float factor = 1.0f / yy;
        for(std::int32_t x = 0; x < table.mapWidth; ++x) {
            const float xx = static_cast<float>(x) + 0.5f;
            const std::int32_t direction = static_cast<std::int32_t>(
                std::atan(xx * factor) *
                ((1.0 / (kPi / 2.0)) * kRippleDirectionPrecision));
            const std::int32_t distance =
                static_cast<std::int32_t>(std::sqrt(xx * xx + yy * yy)) &
                rippleMask;
            table.displacement[static_cast<std::size_t>(y) * table.mapWidth + x] =
                static_cast<std::uint16_t>(distance * kRippleDirectionPrecision +
                                           direction);
        }
    }

    const std::size_t driftRows =
        static_cast<std::size_t>(p.maxDrift) * kRippleDriftPrecision;
    const std::size_t driftStride =
        static_cast<std::size_t>(p.rippleWidth) * kRippleDirectionPrecision;
    if(driftRows && driftStride > std::numeric_limits<std::size_t>::max() / driftRows)
        throw std::overflow_error("ripple drift table too large");
    table.drift.resize(driftRows * driftStride);
    if(p.maxDrift == 0)
        return table;

    std::vector<std::int32_t> rippleForm(p.rippleWidth);
    for(std::int32_t w = 0; w < p.rippleWidth; ++w) {
        const float radians = static_cast<float>(w) / p.rippleWidth *
                              static_cast<float>(kPi * -2.0);
        float s = (std::sin(radians) + std::sin(radians * 2.0f - 2.0f) * 0.2f) /
                  1.19f;
        s *= s;
        s = std::clamp(s, -1.0f, 1.0f);
        rippleForm[w] = RoundFixed(s * 2048.0f);
    }

    std::array<std::int32_t, kRippleDirectionPrecision> cosine{};
    std::array<std::int32_t, kRippleDirectionPrecision> sine{};
    for(std::int32_t direction = 0; direction < kRippleDirectionPrecision;
        ++direction) {
        const float angle = static_cast<float>(kPi * 0.5) -
                            (static_cast<float>(direction) + 0.5f) *
                                static_cast<float>((kPi / 2.0) /
                                                   kRippleDirectionPrecision);
        cosine[direction] = RoundFixed(std::cos(angle) * 2048.0f);
        sine[direction] = RoundFixed(std::sin(angle) * 2048.0f);
    }

    for(std::int32_t drift = 0;
        drift < p.maxDrift * kRippleDriftPrecision; ++drift) {
        const std::int32_t fixedDrift =
            (drift << 10) / kRippleDriftPrecision;
        for(std::int32_t w = 0; w < p.rippleWidth; ++w) {
            const std::int32_t fixedDistance =
                static_cast<std::int32_t>(
                    static_cast<std::int64_t>(rippleForm[w]) * fixedDrift >> 10);
            for(std::int32_t direction = 0;
                direction < kRippleDirectionPrecision; ++direction) {
                const std::int32_t xd = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(cosine[direction]) * fixedDistance >>
                    11);
                const std::int32_t yd = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(sine[direction]) * fixedDistance >>
                    11);
                const auto xOffset = static_cast<std::uint8_t>(
                    static_cast<std::int8_t>(xd >> 11));
                const auto yOffset = static_cast<std::uint8_t>(
                    static_cast<std::int8_t>(yd >> 11));
                const std::size_t index =
                    (static_cast<std::size_t>(drift) * p.rippleWidth + w) *
                        kRippleDirectionPrecision +
                    direction;
                table.drift[index] =
                    static_cast<std::uint16_t>((xOffset << 8) | yOffset);
            }
        }
    }
    return table;
}

RippleFrame ComputeRippleFrame(std::uint64_t currentTime,
                               std::uint64_t duration,
                               const RippleParameters &p) {
    RippleFrame frame;
    if(duration == 0)
        duration = 1;
    frame.blendRatio = static_cast<std::int32_t>(
        static_cast<unsigned __int128>(currentTime) * 255 / duration);
    frame.blendRatio = std::min(frame.blendRatio, 255);

    const double phaseValue =
        p.speed * ((1.0 / (kPi * 2.0)) * (1.0 / 1000.0)) *
        static_cast<double>(currentTime) * p.rippleWidth;
    std::int32_t phase = TruncateToInt(phaseValue) % p.rippleWidth;
    if(phase < 0)
        phase = 0; // preserve the original's unusual negative-speed behavior
    frame.phase = p.rippleWidth - phase - 1;

    if(p.maxDrift == 0) {
        frame.drift = 0;
        return frame;
    }
    frame.drift = TruncateToInt(
        std::sin(kPi * static_cast<double>(currentTime) / duration) *
        p.maxDrift * kRippleDriftPrecision);
    frame.drift = std::clamp(frame.drift, 0,
                             p.maxDrift * kRippleDriftPrecision - 1);
    return frame;
}

std::uint32_t PackUnsigned16Pair(std::uint16_t first, std::uint16_t second) {
    return static_cast<std::uint32_t>(first & 0xffu) |
           (static_cast<std::uint32_t>(first >> 8) << 8) |
           (static_cast<std::uint32_t>(second & 0xffu) << 16) |
           (static_cast<std::uint32_t>(second >> 8) << 24);
}

std::uint32_t PackSigned24(std::int32_t value) {
    const std::int64_t wide = value;
    const std::uint32_t magnitude = static_cast<std::uint32_t>(
        wide < 0 ? -wide : wide);
    if(magnitude > 0x00ffffffu)
        throw std::overflow_error("signed LUT value exceeds 24-bit magnitude");
    return (magnitude & 0xffu) | ((magnitude & 0xff00u)) |
           ((magnitude & 0xff0000u)) | (value < 0 ? 0xff000000u : 0u);
}

std::int32_t UnpackSigned24(std::uint32_t packed) {
    const std::int32_t magnitude = static_cast<std::int32_t>(packed & 0x00ffffffu);
    return (packed >> 24) ? -magnitude : magnitude;
}

} // namespace extrans
