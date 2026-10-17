#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace NicheGraphics::Drivers::PackedPixels
{

constexpr bool validDepth(uint8_t depth)
{
    return depth == 1 || depth == 2 || depth == 4;
}

constexpr bool supports(uint8_t supportedDepths, uint8_t depth)
{
    return validDepth(depth) && (supportedDepths & depth);
}

constexpr uint8_t levels(uint8_t depth)
{
    return validDepth(depth) ? static_cast<uint8_t>(1U << depth) : 0;
}

constexpr uint8_t maxValue(uint8_t depth)
{
    return validDepth(depth) ? static_cast<uint8_t>(levels(depth) - 1) : 0;
}

constexpr uint8_t pixelsPerByte(uint8_t depth)
{
    return validDepth(depth) ? static_cast<uint8_t>(8 / depth) : 0;
}

constexpr size_t rowBytes(uint16_t width, uint8_t depth)
{
    return validDepth(depth) ? (static_cast<size_t>(width) * depth + 7) / 8 : 0;
}

inline uint8_t readMsb(uint8_t packed, uint8_t pixel, uint8_t depth)
{
    const uint8_t perByte = pixelsPerByte(depth);
    if (!perByte || pixel >= perByte)
        return 0;
    const uint8_t shift = (perByte - 1 - pixel) * depth;
    return (packed >> shift) & maxValue(depth);
}

inline void writeMsb(uint8_t &packed, uint8_t pixel, uint8_t depth, uint8_t value)
{
    const uint8_t perByte = pixelsPerByte(depth);
    if (!perByte || pixel >= perByte)
        return;
    const uint8_t shift = (perByte - 1 - pixel) * depth;
    const uint8_t mask = maxValue(depth) << shift;
    const uint8_t clamped = value > maxValue(depth) ? maxValue(depth) : value;
    packed = (packed & ~mask) | (clamped << shift);
}

inline uint8_t packedByte(uint8_t depth, uint8_t value)
{
    uint8_t packed = 0;
    const uint8_t perByte = pixelsPerByte(depth);
    for (uint8_t pixel = 0; pixel < perByte; ++pixel)
        writeMsb(packed, pixel, depth, value);
    return packed;
}

inline void fillSpanMsb(uint8_t *row, uint16_t start, uint16_t end, uint8_t depth, uint8_t value)
{
    const uint8_t perByte = pixelsPerByte(depth);
    if (!row || !perByte || start >= end)
        return;

    while (start < end && start % perByte) {
        writeMsb(row[start / perByte], start % perByte, depth, value);
        ++start;
    }

    const size_t fullBytes = (end - start) / perByte;
    if (fullBytes) {
        memset(row + start / perByte, packedByte(depth, value), fullBytes);
        start += fullBytes * perByte;
    }

    while (start < end) {
        writeMsb(row[start / perByte], start % perByte, depth, value);
        ++start;
    }
}

} // namespace NicheGraphics::Drivers::PackedPixels
