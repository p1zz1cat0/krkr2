// PackinOne.dll layerExImage compatibility surface.
//
// The six methods in this file are the only layerExImage members confirmed by
// the latest PackinOne fixture.  The pixel algorithms are based on the
// official krkrz/layerExImage reference implementation and the CxImage
// 7.0.2-derived
// routines it carries.  CxImage notices are retained in the source tree's
// third-party notice; this file does not import the old Windows codec stack.

#include "layerExImage.h"

#include "layerExBase.hpp"
#include "MsgIntf.h"
#include "ncbind.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("packinone.dll")

namespace packinone::layerExImage {

bool ValidateGaussianBlurRadius(double raw, float &normalized) {
    if(!std::isfinite(raw) || std::fabs(raw) > kMaxGaussianBlurRadius)
        return false;

    const float narrowed = static_cast<float>(raw);
    if(!std::isfinite(narrowed))
        return false;

    normalized = static_cast<float>(std::fabs(narrowed));
    return std::isfinite(normalized);
}

void GenerateWhiteNoise(unsigned char *buffer, int width, int height,
                        std::ptrdiff_t pitch) {
    if(!buffer || width <= 0 || height <= 0)
        return;

    for(int y = 0; y < height; ++y) {
        unsigned char *pixel = buffer + static_cast<std::ptrdiff_t>(y) * pitch;
        for(int x = 0; x < width; ++x, pixel += 4) {
            // Keep the reference's rand()-based, non-deterministic behavior.
            const unsigned char value = static_cast<unsigned char>(
                std::rand() / (RAND_MAX / 255));
            pixel[0] = value; // B
            pixel[1] = value; // G
            pixel[2] = value; // R
            // pixel[3] is the original alpha and is intentionally untouched.
        }
    }
}

} // namespace packinone::layerExImage

namespace {

using Byte = unsigned char;
using MatrixIndex = std::int32_t;

struct RGBQuad {
    Byte blue;
    Byte green;
    Byte red;
};

void ThrowInvalidImage() {
    TVPThrowExceptionMessage(
        TJS_W("PackinOne layerExImage requires a valid Layer image."));
}

template <typename T>
T ClampByteValue(T value) {
    return std::max<T>(0, std::min<T>(255, value));
}

static RGBQuad RGBToHSL(RGBQuad color) {
    constexpr int hslMax = 255;
    constexpr int rgbMax = 255;
    constexpr Byte hslUndefined = static_cast<Byte>(hslMax * 2 / 3);

    const Byte maxValue = std::max({color.red, color.green, color.blue});
    const Byte minValue = std::min({color.red, color.green, color.blue});
    const Byte light = static_cast<Byte>((((maxValue + minValue) * hslMax) +
                                           rgbMax) /
                                          (2 * rgbMax));

    Byte hue = hslUndefined;
    Byte saturation = 0;
    if(maxValue != minValue) {
        if(light <= hslMax / 2)
            saturation = static_cast<Byte>((((maxValue - minValue) * hslMax) +
                                             ((maxValue + minValue) / 2)) /
                                            (maxValue + minValue));
        else
            saturation = static_cast<Byte>((((maxValue - minValue) * hslMax) +
                                             ((2 * rgbMax - maxValue -
                                               minValue) /
                                              2)) /
                                            (2 * rgbMax - maxValue - minValue));

        const int range = maxValue - minValue;
        const int redDelta = (((maxValue - color.red) * (hslMax / 6)) +
                              (range / 2)) /
                             range;
        const int greenDelta = (((maxValue - color.green) * (hslMax / 6)) +
                                (range / 2)) /
                               range;
        const int blueDelta = (((maxValue - color.blue) * (hslMax / 6)) +
                               (range / 2)) /
                              range;

        int hueValue = 0;
        if(color.red == maxValue)
            hueValue = blueDelta - greenDelta;
        else if(color.green == maxValue)
            hueValue = hslMax / 3 + redDelta - blueDelta;
        else
            hueValue = (2 * hslMax) / 3 + greenDelta - redDelta;

        hue = static_cast<Byte>(hueValue);
    }

    return {light, saturation, hue};
}

static float HueToRGB(float n1, float n2, float hue) {
    if(hue > 360.0f)
        hue -= 360.0f;
    else if(hue < 0.0f)
        hue += 360.0f;

    if(hue < 60.0f)
        return n1 + (n2 - n1) * hue / 60.0f;
    if(hue < 180.0f)
        return n2;
    if(hue < 240.0f)
        return n1 + (n2 - n1) * (240.0f - hue) / 60.0f;
    return n1;
}

static RGBQuad HSLToRGB(RGBQuad hsl) {
    const float hue = static_cast<float>(hsl.red) * 360.0f / 255.0f;
    const float saturation = static_cast<float>(hsl.green) / 255.0f;
    const float light = static_cast<float>(hsl.blue) / 255.0f;

    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    if(saturation == 0.0f) {
        red = green = blue = light * 255.0f;
    } else {
        const float m2 = light <= 0.5f
                             ? light * (1.0f + saturation)
                             : light + saturation - light * saturation;
        const float m1 = 2.0f * light - m2;
        red = HueToRGB(m1, m2, hue + 120.0f) * 255.0f;
        green = HueToRGB(m1, m2, hue) * 255.0f;
        blue = HueToRGB(m1, m2, hue - 120.0f) * 255.0f;
    }

    return {static_cast<Byte>(ClampByteValue(static_cast<int>(blue))),
            static_cast<Byte>(ClampByteValue(static_cast<int>(green))),
            static_cast<Byte>(ClampByteValue(static_cast<int>(red)))};
}

static int HueToRGBInt(double n1, double n2, double hue) {
    if(hue < 0.0)
        hue += 1.0;
    else if(hue > 1.0)
        hue -= 1.0;

    double color = 0.0;
    if(hue < 1.0 / 6.0)
        color = n1 + (n2 - n1) * hue * 6.0;
    else if(hue < 1.0 / 2.0)
        color = n2;
    else if(hue < 2.0 / 3.0)
        color = n1 + (n2 - n1) * (2.0 / 3.0 - hue) * 6.0;
    else
        color = n1;
    return ClampByteValue(static_cast<int>(color * 255.0));
}

static void ModulatePixel(int &blue, int &green, int &red, double hueOffset,
                          double saturationOffset, double lightnessOffset) {
    const double r = red / 255.0;
    const double g = green / 255.0;
    const double b = blue / 255.0;
    const double maxValue = std::max({r, g, b});
    const double minValue = std::min({r, g, b});
    const double delta = maxValue - minValue;
    const double sum = maxValue + minValue;
    double lightness = sum / 2.0;
    double hue = 0.0;
    double saturation = 0.0;

    if(delta != 0.0) {
        saturation = lightness < 0.5 ? delta / sum : delta / (2.0 - sum);
        if(r == maxValue)
            hue = (g - b) / delta;
        else if(g == maxValue)
            hue = 2.0 + (b - r) / delta;
        else
            hue = 4.0 + (r - g) / delta;
        hue /= 6.0;
    }

    hue += hueOffset;
    while(hue < 0.0)
        hue += 1.0;
    while(hue > 1.0)
        hue -= 1.0;

    if(saturationOffset > 0.0)
        saturation += (1.0 - saturation) * saturationOffset;
    else
        saturation += saturation * saturationOffset;
    if(lightnessOffset > 0.0)
        lightness += (1.0 - lightness) * lightnessOffset;
    else
        lightness += lightness * lightnessOffset;

    if(saturation == 0.0) {
        red = green = blue = ClampByteValue(static_cast<int>(lightness * 255.0));
        return;
    }

    const double m2 = lightness <= 0.5
                          ? lightness * (1.0 + saturation)
                          : lightness + saturation - lightness * saturation;
    const double m1 = 2.0 * lightness - m2;
    red = HueToRGBInt(m1, m2, hue + 1.0 / 3.0);
    green = HueToRGBInt(m1, m2, hue);
    blue = HueToRGBInt(m1, m2, hue - 1.0 / 3.0);
}

static bool CheckedImageBytes(int width, int height, std::size_t &bytes) {
    if(width <= 0 || height <= 0)
        return false;
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    if(w > std::numeric_limits<std::size_t>::max() / 4)
        return false;
    const std::size_t rowBytes = w * 4;
    if(h > std::numeric_limits<std::size_t>::max() / rowBytes)
        return false;
    bytes = rowBytes * h;
    return true;
}

static bool GenerateConvolutionMatrix(float normalizedRadius,
                                      std::vector<float> &matrix) {
    const double standardDeviation =
        std::fabs(static_cast<double>(normalizedRadius)) * 0.5 + 0.25;
    const double effectRadius = standardDeviation * 2.0;
    const double halfSpan = std::ceil(effectRadius - 0.5);
    if(!std::isfinite(halfSpan) || halfSpan < 0.0 ||
       halfSpan > (std::numeric_limits<MatrixIndex>::max() - 1) / 2)
        return false;

    const MatrixIndex length =
        static_cast<MatrixIndex>(2 * static_cast<MatrixIndex>(halfSpan) + 1);
    if(length <= 0)
        return false;

    try {
        matrix.assign(static_cast<std::size_t>(length), 0.0f);
    } catch(const std::bad_alloc &) {
        return false;
    }

    const MatrixIndex midpoint = length / 2;
    const float stdDev = static_cast<float>(standardDeviation);
    const float radius = static_cast<float>(effectRadius);
    for(MatrixIndex i = midpoint + 1; i < length; ++i) {
        const float baseX = static_cast<float>(i - midpoint) - 0.5f;
        float sum = 0.0f;
        for(int j = 1; j <= 50; ++j) {
            const float sample = baseX + 0.02f * static_cast<float>(j);
            if(sample <= radius)
                sum += std::exp(-(sample * sample) /
                                (2.0f * stdDev * stdDev));
        }
        matrix[static_cast<std::size_t>(i)] = sum / 50.0f;
    }

    for(MatrixIndex i = 0; i <= midpoint; ++i)
        matrix[static_cast<std::size_t>(i)] =
            matrix[static_cast<std::size_t>(length - 1 - i)];

    float centerSum = 0.0f;
    for(int j = 0; j <= 50; ++j) {
        const float sample = 0.5f + 0.02f * static_cast<float>(j);
        centerSum += std::exp(-(sample * sample) /
                              (2.0f * stdDev * stdDev));
    }
    matrix[static_cast<std::size_t>(midpoint)] = centerSum / 51.0f;

    float total = 0.0f;
    for(const float value : matrix)
        total += value;
    if(!std::isfinite(total) || total <= 0.0f)
        return false;
    for(float &value : matrix)
        value /= total;
    return true;
}

static bool GenerateLookupTable(const std::vector<float> &matrix,
                                std::vector<float> &table) {
    if(matrix.empty() || matrix.size() >
                           std::numeric_limits<std::size_t>::max() / 256)
        return false;
    try {
        table.resize(matrix.size() * 256);
    } catch(const std::bad_alloc &) {
        return false;
    }
    for(std::size_t i = 0; i < matrix.size(); ++i)
        for(std::size_t value = 0; value < 256; ++value)
            table[i * 256 + value] = matrix[i] * static_cast<float>(value);
    return true;
}

static void BlurLine(const std::vector<float> &table,
                     const std::vector<float> &matrix, const Byte *source,
                     Byte *destination, int length, int bytesPerPixel) {
    const MatrixIndex matrixLength = static_cast<MatrixIndex>(matrix.size());
    const MatrixIndex middle = matrixLength / 2;
    if(matrixLength > length) {
        for(int row = 0; row < length; ++row) {
            float scale = 0.0f;
            for(int j = 0; j < length; ++j) {
                const int index = j + middle - row;
                if(index >= 0 && index < matrixLength)
                    scale += matrix[static_cast<std::size_t>(index)];
            }
            for(int channel = 0; channel < bytesPerPixel; ++channel) {
                float sum = 0.0f;
                for(int j = 0; j < length; ++j) {
                    if(j >= row - middle && j <= row + middle)
                        sum += source[j * bytesPerPixel + channel] *
                               matrix[static_cast<std::size_t>(j)];
                }
                destination[row * bytesPerPixel + channel] =
                    static_cast<Byte>(0.5f + sum / scale);
            }
        }
        return;
    }

    int row = 0;
    for(; row < middle; ++row) {
        float scale = 0.0f;
        for(int j = middle - row; j < matrixLength; ++j)
            scale += matrix[static_cast<std::size_t>(j)];
        for(int channel = 0; channel < bytesPerPixel; ++channel) {
            float sum = 0.0f;
            for(int j = middle - row; j < matrixLength; ++j)
                sum += source[(row + j - middle) * bytesPerPixel + channel] *
                       matrix[static_cast<std::size_t>(j)];
            destination[row * bytesPerPixel + channel] =
                static_cast<Byte>(0.5f + sum / scale);
        }
    }

    for(; row < length - middle; ++row) {
        const Byte *sourcePixel = source + (row - middle) * bytesPerPixel;
        Byte *destinationPixel = destination + row * bytesPerPixel;
        for(int channel = 0; channel < bytesPerPixel; ++channel) {
            float sum = 0.0f;
            for(MatrixIndex j = 0; j < matrixLength; ++j)
                sum += table[static_cast<std::size_t>(j) * 256 +
                             sourcePixel[j * bytesPerPixel + channel]];
            destinationPixel[channel] = static_cast<Byte>(0.5f + sum);
        }
    }

    for(; row < length; ++row) {
        float scale = 0.0f;
        for(int j = 0; j < length - row + middle; ++j)
            scale += matrix[static_cast<std::size_t>(j)];
        for(int channel = 0; channel < bytesPerPixel; ++channel) {
            float sum = 0.0f;
            for(int j = 0; j < length - row + middle; ++j)
                sum += source[(row + j - middle) * bytesPerPixel + channel] *
                       matrix[static_cast<std::size_t>(j)];
            destination[row * bytesPerPixel + channel] =
                static_cast<Byte>(0.5f + sum / scale);
        }
    }
}

static void GetColumn(const Byte *source, Byte *destination, int height,
                      int pitch) {
    for(int row = 0; row < height; ++row) {
        std::copy_n(source, 4, destination);
        source += pitch;
        destination += 4;
    }
}

static void SetColumn(Byte *destination, const Byte *source, int height,
                      int pitch) {
    for(int row = 0; row < height; ++row) {
        std::copy_n(source, 4, destination);
        source += 4;
        destination += pitch;
    }
}

class layerExImage : public layerExBase {
public:
    explicit layerExImage(DispatchT object) : layerExBase(object) {}

    void reset() override {
        layerExBase::reset();
        _buffer += static_cast<std::ptrdiff_t>(_clipTop) * _pitch +
                   static_cast<std::ptrdiff_t>(_clipLeft) * 4;
        _width = _clipWidth;
        _height = _clipHeight;
    }

    bool IsValidImage() const {
        // layerExBase exposes the first (top) scanline.  The native Layer
        // implementation is top-down, so reject a negative pitch instead of
        // walking backwards from an unnormalised origin.
        if(!_buffer || _width <= 0 || _height <= 0 || _pitch <= 0)
            return false;
        const std::int64_t rowBytes = static_cast<std::int64_t>(_width) * 4;
        const std::int64_t pitch = static_cast<std::int64_t>(_pitch);
        return rowBytes > 0 && pitch >= rowBytes;
    }

    void light(int brightness, int contrast) {
        if(!IsValidImage())
            ThrowInvalidImage();

        const double contrastScale = (100.0 + contrast) / 100.0;
        const double brightnessOffset = static_cast<double>(brightness) + 128.0;
        Byte table[256];
        for(int value = 0; value < 256; ++value) {
            const double adjusted =
                (value - 128.0) * contrastScale + brightnessOffset;
            table[value] = static_cast<Byte>(ClampByteValue(
                static_cast<int>(adjusted)));
        }

        for(int y = 0; y < _height; ++y) {
            Byte *pixel = _buffer + static_cast<std::ptrdiff_t>(y) * _pitch;
            for(int x = 0; x < _width; ++x, pixel += 4) {
                pixel[0] = table[pixel[0]];
                pixel[1] = table[pixel[1]];
                pixel[2] = table[pixel[2]];
            }
        }
        redraw();
    }

    void colorize(int hue, int saturation, double blend) {
        if(!IsValidImage())
            ThrowInvalidImage();
        blend = std::clamp(blend, 0.0, 1.0);
        const int blend256 = static_cast<int>(256.0 * blend);
        const int inverseBlend = 256 - blend256;
        const bool fullBlend = blend > 0.999;

        for(int y = 0; y < _height; ++y) {
            Byte *pixel = _buffer + static_cast<std::ptrdiff_t>(y) * _pitch;
            for(int x = 0; x < _width; ++x, pixel += 4) {
                RGBQuad color{pixel[0], pixel[1], pixel[2]};
                if(fullBlend) {
                    RGBQuad hsl = RGBToHSL(color);
                    hsl.red = static_cast<Byte>(hue);
                    hsl.green = static_cast<Byte>(saturation);
                    color = HSLToRGB(hsl);
                } else {
                    RGBQuad hsl = RGBToHSL(color);
                    hsl.red = static_cast<Byte>(hue);
                    hsl.green = static_cast<Byte>(saturation);
                    const RGBQuad target = HSLToRGB(hsl);
                    color.red = static_cast<Byte>((target.red * blend256 +
                                                   color.red * inverseBlend) >>
                                                  8);
                    color.green = static_cast<Byte>((target.green * blend256 +
                                                     color.green * inverseBlend) >>
                                                    8);
                    color.blue = static_cast<Byte>((target.blue * blend256 +
                                                    color.blue * inverseBlend) >>
                                                   8);
                }
                pixel[0] = color.blue;
                pixel[1] = color.green;
                pixel[2] = color.red;
            }
        }
        redraw();
    }

    void modulate(int hue, int saturation, int luminance) {
        if(!IsValidImage())
            ThrowInvalidImage();
        const double hueOffset = hue / 360.0;
        const double saturationOffset = saturation / 100.0;
        const double lightnessOffset = luminance / 100.0;

        for(int y = 0; y < _height; ++y) {
            Byte *pixel = _buffer + static_cast<std::ptrdiff_t>(y) * _pitch;
            for(int x = 0; x < _width; ++x, pixel += 4) {
                int blue = pixel[0];
                int green = pixel[1];
                int red = pixel[2];
                ModulatePixel(blue, green, red, hueOffset, saturationOffset,
                              lightnessOffset);
                pixel[0] = static_cast<Byte>(blue);
                pixel[1] = static_cast<Byte>(green);
                pixel[2] = static_cast<Byte>(red);
            }
        }
        redraw();
    }

    void noise(int level) {
        if(!IsValidImage())
            ThrowInvalidImage();
        for(int y = 0; y < _height; ++y) {
            Byte *pixel = _buffer + static_cast<std::ptrdiff_t>(y) * _pitch;
            for(int x = 0; x < _width; ++x, pixel += 4) {
                for(int channel = 0; channel < 3; ++channel) {
                    const auto offset = static_cast<std::int64_t>(
                        (std::rand() / static_cast<float>(RAND_MAX) - 0.5f) *
                        static_cast<float>(level));
                    const auto adjusted =
                        static_cast<std::int64_t>(pixel[channel]) + offset;
                    pixel[channel] = static_cast<Byte>(ClampByteValue(
                        std::clamp<std::int64_t>(adjusted, 0, 255)));
                }
            }
        }
        redraw();
    }

    void generateWhiteNoise() {
        if(!IsValidImage())
            ThrowInvalidImage();
        packinone::layerExImage::GenerateWhiteNoise(
            _buffer, _width, _height, _pitch);
        redraw();
    }

    tjs_error gaussianBlur(double rawRadius) {
        float radius = 0.0f;
        if(!packinone::layerExImage::ValidateGaussianBlurRadius(rawRadius,
                                                                   radius))
            return TJS_E_INVALIDPARAM;
        if(!IsValidImage())
            return TJS_E_INVALIDPARAM;

        std::size_t imageBytes = 0;
        if(!CheckedImageBytes(_width, _height, imageBytes))
            return TJS_E_INVALIDPARAM;

        std::vector<float> matrix;
        std::vector<float> lookup;
        std::vector<Byte> horizontal;
        std::vector<Byte> output;
        std::vector<Byte> currentColumn;
        std::vector<Byte> destinationColumn;
        try {
            if(!GenerateConvolutionMatrix(radius, matrix) ||
               !GenerateLookupTable(matrix, lookup))
                return TJS_E_FAIL;
            horizontal.resize(imageBytes);
            output.resize(imageBytes);
            currentColumn.resize(static_cast<std::size_t>(_height) * 4);
            destinationColumn.resize(static_cast<std::size_t>(_height) * 4);
        } catch(const std::bad_alloc &) {
            return TJS_E_FAIL;
        } catch(const std::length_error &) {
            return TJS_E_FAIL;
        }

        const std::size_t rowBytes = static_cast<std::size_t>(_width) * 4;
        if(rowBytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return TJS_E_INVALIDPARAM;
        for(int y = 0; y < _height; ++y) {
            const Byte *source =
                _buffer + static_cast<std::ptrdiff_t>(y) * _pitch;
            BlurLine(lookup, matrix, source,
                     horizontal.data() + static_cast<std::size_t>(y) * rowBytes,
                     _width, 4);
        }

        for(int x = 0; x < _width; ++x) {
            GetColumn(horizontal.data() + static_cast<std::size_t>(x) * 4,
                      currentColumn.data(), _height,
                      static_cast<int>(rowBytes));
            BlurLine(lookup, matrix, currentColumn.data(),
                     destinationColumn.data(), _height, 4);
            SetColumn(output.data() + static_cast<std::size_t>(x) * 4,
                      destinationColumn.data(), _height,
                      static_cast<int>(rowBytes));
        }

        // Commit only after all validation, allocations and convolution work
        // have completed. Invalid input and allocation failure never mutate
        // the layer's pixel buffer.
        for(int y = 0; y < _height; ++y) {
            Byte *destination =
                _buffer + static_cast<std::ptrdiff_t>(y) * _pitch;
            std::copy_n(output.data() + static_cast<std::size_t>(y) * rowBytes,
                        rowBytes, destination);
        }
        redraw();
        return TJS_S_OK;
    }
};

void EnsureLayerClassRegistered() {
    tTJSVariant layerClass;
    TVPExecuteExpression(TJS_W("Layer"), &layerClass);
    if(layerClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("PackinOne layerExImage requires the Layer class."));
}

tjs_error GaussianBlurCallback(tTJSVariant *, tjs_int numparams,
                               tTJSVariant **param,
                               layerExImage *nativeInstance) {
    if(numparams < 1 || !param || !param[0] || !nativeInstance)
        return numparams < 1 ? TJS_E_BADPARAMCOUNT : TJS_E_INVALIDPARAM;

    if(param[0]->Type() != tvtInteger && param[0]->Type() != tvtReal)
        return TJS_E_INVALIDTYPE;

    const double rawRadius = static_cast<double>(param[0]->AsReal());
    const tjs_error result = nativeInstance->gaussianBlur(rawRadius);
    if(TJS_FAILED(result)) {
        if(auto logger = spdlog::get("plugin"))
            logger->warn("[packinone] layerExImage.gaussianBlur rejected "
                         "non-finite, out-of-range, or unsafe input");
    }
    return result;
}

} // namespace

extern "C" void TVPPackinOneLayerExImageAnchor() {}

NCB_PRE_REGIST_CALLBACK(EnsureLayerClassRegistered);

NCB_GET_INSTANCE_HOOK(layerExImage) {
    NCB_INSTANCE_GETTER(objthis) {
        ClassT *instance = GetNativeInstance(objthis);
        if(!instance) {
            instance = new ClassT(objthis);
            SetNativeInstance(objthis, instance);
        }
        instance->reset();
        return instance;
    }
    ~NCB_GET_INSTANCE_HOOK_CLASS() {}
};

NCB_ATTACH_CLASS_WITH_HOOK(layerExImage, Layer) {
    NCB_METHOD(light);
    NCB_METHOD(colorize);
    NCB_METHOD(modulate);
    NCB_METHOD(noise);
    NCB_METHOD(generateWhiteNoise);
    NCB_METHOD_RAW_CALLBACK(gaussianBlur, GaussianBlurCallback, 0);
}
