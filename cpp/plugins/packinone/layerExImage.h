#pragma once

#include <cstddef>

namespace packinone::layerExImage {

// This is a resource-safety limit for the native macOS implementation.  It
// is not inferred from the Windows DLL ABI.
inline constexpr double kMaxGaussianBlurRadius = 64.0;

// Validate the real value before it is narrowed to the fixture's float ABI.
// Finite negative values retain the reference implementation's fabs() rule.
bool ValidateGaussianBlurRadius(double raw, float &normalized);

// CxImage's generateWhiteNoise behavior: random grayscale RGB, untouched A.
// The helper deliberately has no deterministic output contract.
void GenerateWhiteNoise(unsigned char *buffer, int width, int height,
                        std::ptrdiff_t pitch);

} // namespace packinone::layerExImage
