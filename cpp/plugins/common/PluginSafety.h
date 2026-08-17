#ifndef YOGHOURT_PLUGIN_SAFETY_H
#define YOGHOURT_PLUGIN_SAFETY_H

#include "tjs.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace pluginSafety {

constexpr std::size_t kMiB = 1024u * 1024u;
constexpr std::size_t kDefaultSingleAllocationLimit = 64u * kMiB;
constexpr std::size_t kDefaultOperationLimit = 256u * kMiB;
constexpr tjs_uint64 kDefaultMinimumDurationMs = 2;
constexpr tjs_uint64 kDefaultMaximumDurationMs = 86'400'000;

enum class SafetyError {
    none,
    invalidType,
    outOfRange,
    nonFinite,
    arithmeticOverflow,
    allocationLimitExceeded,
    operationBudgetExceeded,
    invalidLayer,
    missingProperty,
    nullBuffer,
    invalidDimensions,
    invalidPitch,
    rowOutOfRange,
};

template <typename T> struct SafetyResult {
    T value{};
    SafetyError error = SafetyError::none;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == SafetyError::none;
    }

    [[nodiscard]] static constexpr SafetyResult success(T result) noexcept {
        return SafetyResult{std::move(result), SafetyError::none};
    }

    [[nodiscard]] static constexpr SafetyResult failure(SafetyError reason) noexcept {
        return SafetyResult{T{}, reason};
    }
};

inline SafetyResult<tjs_int64>
readBoundedInteger(const tTJSVariant &variant, tjs_int64 minimum,
                   tjs_int64 maximum) noexcept {
    if(variant.Type() != tvtInteger)
        return SafetyResult<tjs_int64>::failure(SafetyError::invalidType);
    const auto value = static_cast<tjs_int64>(variant);
    if(value < minimum || value > maximum)
        return SafetyResult<tjs_int64>::failure(SafetyError::outOfRange);
    return SafetyResult<tjs_int64>::success(value);
}

inline SafetyResult<double>
readFiniteReal(const tTJSVariant &variant, double minimum,
               double maximum) noexcept {
    if(variant.Type() != tvtInteger && variant.Type() != tvtReal)
        return SafetyResult<double>::failure(SafetyError::invalidType);
    const auto value = static_cast<double>(variant);
    if(!std::isfinite(value))
        return SafetyResult<double>::failure(SafetyError::nonFinite);
    if(value < minimum || value > maximum)
        return SafetyResult<double>::failure(SafetyError::outOfRange);
    return SafetyResult<double>::success(value);
}

inline SafetyResult<tjs_uint64>
readDuration(const tTJSVariant &variant,
             tjs_uint64 minimum = kDefaultMinimumDurationMs,
             tjs_uint64 maximum = kDefaultMaximumDurationMs) noexcept {
    if(minimum > maximum || maximum >
                                static_cast<tjs_uint64>(
                                    std::numeric_limits<tjs_int64>::max()))
        return SafetyResult<tjs_uint64>::failure(SafetyError::outOfRange);
    const auto result = readBoundedInteger(
        variant, static_cast<tjs_int64>(minimum),
        static_cast<tjs_int64>(maximum));
    if(!result)
        return SafetyResult<tjs_uint64>::failure(result.error);
    return SafetyResult<tjs_uint64>::success(
        static_cast<tjs_uint64>(result.value));
}

inline SafetyResult<std::size_t> checkedAdd(std::size_t left,
                                            std::size_t right) noexcept {
    if(right > std::numeric_limits<std::size_t>::max() - left)
        return SafetyResult<std::size_t>::failure(
            SafetyError::arithmeticOverflow);
    return SafetyResult<std::size_t>::success(left + right);
}

inline SafetyResult<std::size_t> checkedMultiply(std::size_t left,
                                                 std::size_t right) noexcept {
    if(left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
        return SafetyResult<std::size_t>::failure(
            SafetyError::arithmeticOverflow);
    return SafetyResult<std::size_t>::success(left * right);
}

struct CheckedElementBytes {
    std::size_t elementCount = 0;
    std::size_t elementSize = 0;
    std::size_t bytes = 0;
};

inline SafetyResult<CheckedElementBytes>
checkedElementBytes(std::size_t elementCount, std::size_t elementSize) noexcept {
    const auto bytes = checkedMultiply(elementCount, elementSize);
    if(!bytes)
        return SafetyResult<CheckedElementBytes>::failure(bytes.error);
    return SafetyResult<CheckedElementBytes>::success(
        CheckedElementBytes{elementCount, elementSize, bytes.value});
}

class OperationBudget {
public:
    explicit constexpr OperationBudget(
        std::size_t limit = kDefaultOperationLimit) noexcept
        : remaining_(limit), consumed_(0) {}

    [[nodiscard]] constexpr std::size_t remaining() const noexcept {
        return remaining_;
    }
    [[nodiscard]] constexpr std::size_t consumed() const noexcept {
        return consumed_;
    }

private:
    friend class ValidatedAllocation;
    friend SafetyResult<class ValidatedAllocation>
    validateAllocationBudget(const CheckedElementBytes &, OperationBudget &,
                             std::size_t) noexcept;

    std::size_t remaining_;
    std::size_t consumed_;
};

class ValidatedAllocation {
public:
    [[nodiscard]] constexpr std::size_t elementCount() const noexcept {
        return elementCount_;
    }
    [[nodiscard]] constexpr std::size_t elementSize() const noexcept {
        return elementSize_;
    }
    [[nodiscard]] constexpr std::size_t bytes() const noexcept { return bytes_; }

private:
    template <typename> friend struct SafetyResult;
    friend SafetyResult<ValidatedAllocation>
    validateAllocationBudget(const CheckedElementBytes &, OperationBudget &,
                             std::size_t) noexcept;

    constexpr ValidatedAllocation(std::size_t elementCount,
                                  std::size_t elementSize,
                                  std::size_t bytes) noexcept
        : elementCount_(elementCount), elementSize_(elementSize), bytes_(bytes) {}
    constexpr ValidatedAllocation() noexcept = default;

    std::size_t elementCount_ = 0;
    std::size_t elementSize_ = 0;
    std::size_t bytes_ = 0;
};

inline SafetyResult<ValidatedAllocation>
validateAllocationBudget(
    const CheckedElementBytes &checked, OperationBudget &operation,
    std::size_t singleAllocationLimit = kDefaultSingleAllocationLimit) noexcept {
    if(checked.bytes > singleAllocationLimit)
        return SafetyResult<ValidatedAllocation>::failure(
            SafetyError::allocationLimitExceeded);
    if(checked.bytes > operation.remaining_)
        return SafetyResult<ValidatedAllocation>::failure(
            SafetyError::operationBudgetExceeded);
    const auto newConsumed = checkedAdd(operation.consumed_, checked.bytes);
    if(!newConsumed)
        return SafetyResult<ValidatedAllocation>::failure(newConsumed.error);

    operation.remaining_ -= checked.bytes;
    operation.consumed_ = newConsumed.value;
    return SafetyResult<ValidatedAllocation>::success(ValidatedAllocation(
        checked.elementCount, checked.elementSize, checked.bytes));
}

struct LayerLayout {
    tjs_int width = 0;
    tjs_int height = 0;
    tjs_int pitchBytes = 0;
    std::size_t rowBytes = 0;
    std::size_t spanBytes = 0;
};

inline SafetyResult<LayerLayout> validateLayerLayout(tjs_int width,
                                                     tjs_int height,
                                                     tjs_int pitchBytes) noexcept {
    if(width <= 0 || height <= 0)
        return SafetyResult<LayerLayout>::failure(
            SafetyError::invalidDimensions);

    const auto rowBytes = checkedElementBytes(static_cast<std::size_t>(width),
                                              sizeof(tjs_uint32));
    if(!rowBytes)
        return SafetyResult<LayerLayout>::failure(rowBytes.error);

    // KrKr2 currently exposes GetScanLine(0) as mainImageBuffer and uses
    // row(y) = row0 + y * positivePitch. A negative pitch is rejected until
    // the core provides an allocation-origin/span contract that proves it safe.
    if(pitchBytes <= 0 ||
       static_cast<std::size_t>(pitchBytes) < rowBytes.value.bytes)
        return SafetyResult<LayerLayout>::failure(SafetyError::invalidPitch);

    const auto lastRowOffset = checkedMultiply(
        static_cast<std::size_t>(height - 1),
        static_cast<std::size_t>(pitchBytes));
    if(!lastRowOffset)
        return SafetyResult<LayerLayout>::failure(lastRowOffset.error);
    const auto spanBytes = checkedAdd(lastRowOffset.value, rowBytes.value.bytes);
    if(!spanBytes)
        return SafetyResult<LayerLayout>::failure(spanBytes.error);
    if(spanBytes.value > kDefaultOperationLimit)
        return SafetyResult<LayerLayout>::failure(
            SafetyError::allocationLimitExceeded);

    return SafetyResult<LayerLayout>::success(LayerLayout{
        width, height, pitchBytes, rowBytes.value.bytes, spanBytes.value});
}

namespace detail {

inline SafetyResult<tjs_int64> readIntegerProperty(iTJSDispatch2 *object,
                                                   const tjs_char *name) noexcept {
    tTJSVariant value;
    if(TJS_FAILED(object->PropGet(0, name, nullptr, &value, object)))
        return SafetyResult<tjs_int64>::failure(SafetyError::missingProperty);
    return readBoundedInteger(value, std::numeric_limits<tjs_int>::min(),
                              std::numeric_limits<tjs_int>::max());
}

inline SafetyResult<tjs_intptr_t>
readPointerProperty(iTJSDispatch2 *object, const tjs_char *name) noexcept {
    tTJSVariant value;
    if(TJS_FAILED(object->PropGet(0, name, nullptr, &value, object)))
        return SafetyResult<tjs_intptr_t>::failure(SafetyError::missingProperty);
    const auto integer = readBoundedInteger(
        value, static_cast<tjs_int64>(std::numeric_limits<tjs_intptr_t>::min()),
        static_cast<tjs_int64>(std::numeric_limits<tjs_intptr_t>::max()));
    if(!integer)
        return SafetyResult<tjs_intptr_t>::failure(integer.error);
    if(integer.value == 0)
        return SafetyResult<tjs_intptr_t>::failure(SafetyError::nullBuffer);
    return SafetyResult<tjs_intptr_t>::success(
        static_cast<tjs_intptr_t>(integer.value));
}

inline SafetyResult<LayerLayout> readLayerLayout(iTJSDispatch2 *object) noexcept {
    if(!object || object->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"),
                                      object) != TJS_S_TRUE)
        return SafetyResult<LayerLayout>::failure(SafetyError::invalidLayer);
    const auto width = readIntegerProperty(object, TJS_W("imageWidth"));
    const auto height = readIntegerProperty(object, TJS_W("imageHeight"));
    const auto pitch =
        readIntegerProperty(object, TJS_W("mainImageBufferPitch"));
    if(!width)
        return SafetyResult<LayerLayout>::failure(width.error);
    if(!height)
        return SafetyResult<LayerLayout>::failure(height.error);
    if(!pitch)
        return SafetyResult<LayerLayout>::failure(pitch.error);
    return validateLayerLayout(static_cast<tjs_int>(width.value),
                               static_cast<tjs_int>(height.value),
                               static_cast<tjs_int>(pitch.value));
}

} // namespace detail

class LayerReadView {
public:
    [[nodiscard]] static SafetyResult<LayerReadView>
    create(iTJSDispatch2 *object) noexcept {
        const auto layout = detail::readLayerLayout(object);
        if(!layout)
            return SafetyResult<LayerReadView>::failure(layout.error);
        const auto pointer =
            detail::readPointerProperty(object, TJS_W("mainImageBuffer"));
        if(!pointer)
            return SafetyResult<LayerReadView>::failure(pointer.error);
        return SafetyResult<LayerReadView>::success(LayerReadView(
            reinterpret_cast<const tjs_uint8 *>(pointer.value), layout.value));
    }

    [[nodiscard]] const tjs_uint8 *pixels() const noexcept { return pixels_; }
    [[nodiscard]] tjs_int width() const noexcept { return layout_.width; }
    [[nodiscard]] tjs_int height() const noexcept { return layout_.height; }
    [[nodiscard]] tjs_int pitchBytes() const noexcept {
        return layout_.pitchBytes;
    }
    [[nodiscard]] SafetyResult<const tjs_uint8 *> row(tjs_int y) const noexcept {
        if(y < 0 || y >= layout_.height)
            return SafetyResult<const tjs_uint8 *>::failure(
                SafetyError::rowOutOfRange);
        return SafetyResult<const tjs_uint8 *>::success(
            pixels_ + static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(layout_.pitchBytes));
    }

private:
    template <typename> friend struct SafetyResult;
    LayerReadView() noexcept = default;
    LayerReadView(const tjs_uint8 *pixels, LayerLayout layout) noexcept
        : pixels_(pixels), layout_(layout) {}

    const tjs_uint8 *pixels_ = nullptr;
    LayerLayout layout_{};
};

class LayerWriteView {
public:
    [[nodiscard]] static SafetyResult<LayerWriteView>
    create(iTJSDispatch2 *object) noexcept {
        const auto layout = detail::readLayerLayout(object);
        if(!layout)
            return SafetyResult<LayerWriteView>::failure(layout.error);
        const auto pointer = detail::readPointerProperty(
            object, TJS_W("mainImageBufferForWrite"));
        if(!pointer)
            return SafetyResult<LayerWriteView>::failure(pointer.error);
        return SafetyResult<LayerWriteView>::success(LayerWriteView(
            reinterpret_cast<tjs_uint8 *>(pointer.value), layout.value));
    }

    [[nodiscard]] tjs_uint8 *pixels() const noexcept { return pixels_; }
    [[nodiscard]] tjs_int width() const noexcept { return layout_.width; }
    [[nodiscard]] tjs_int height() const noexcept { return layout_.height; }
    [[nodiscard]] tjs_int pitchBytes() const noexcept {
        return layout_.pitchBytes;
    }
    [[nodiscard]] SafetyResult<tjs_uint8 *> row(tjs_int y) const noexcept {
        if(y < 0 || y >= layout_.height)
            return SafetyResult<tjs_uint8 *>::failure(
                SafetyError::rowOutOfRange);
        return SafetyResult<tjs_uint8 *>::success(
            pixels_ + static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(layout_.pitchBytes));
    }

private:
    template <typename> friend struct SafetyResult;
    LayerWriteView() noexcept = default;
    LayerWriteView(tjs_uint8 *pixels, LayerLayout layout) noexcept
        : pixels_(pixels), layout_(layout) {}

    tjs_uint8 *pixels_ = nullptr;
    LayerLayout layout_{};
};

} // namespace pluginSafety

#endif
