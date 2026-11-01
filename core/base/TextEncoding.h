#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Detect the encoding of text bytes. Sets bomSize to the BOM length (0 if none).
std::string checkTextEncoding(const void *buf, size_t size,
                              std::uint8_t &bomSize);
