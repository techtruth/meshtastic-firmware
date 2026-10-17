#pragma once

#ifdef MESHTASTIC_INCLUDE_INKHUD

#include <stddef.h>
#include <stdint.h>

#include "graphics/niche/Drivers/EInk/PackedPixels.h"

namespace NicheGraphics::InkHUD::MapTileUtils
{

constexpr uint16_t TILE_SIZE = 256;
constexpr size_t MAX_TILE_BYTES = TILE_SIZE * TILE_SIZE / 2;
constexpr uint32_t HEADER_SIZE = 16;
constexpr uint32_t POSITION_SIZE = 5;
constexpr uint32_t PAYLOAD_SIZE = 7;
constexpr uint8_t LAYOUT_SPARSE = 0;
constexpr uint8_t LAYOUT_GRID = 1;
constexpr uint8_t KIND_LZ4 = 0;
constexpr uint8_t KIND_WHITE = 1;
constexpr uint8_t KIND_BLACK = 2;

struct Format {
    uint8_t bitsPerPixel = 0;

    constexpr bool valid() const { return Drivers::PackedPixels::validDepth(bitsPerPixel); }
    constexpr uint8_t levels() const { return Drivers::PackedPixels::levels(bitsPerPixel); }
    constexpr uint8_t maxValue() const { return Drivers::PackedPixels::maxValue(bitsPerPixel); }
    constexpr uint8_t pixelsPerByte() const { return Drivers::PackedPixels::pixelsPerByte(bitsPerPixel); }
    constexpr size_t tileBytes() const { return valid() ? TILE_SIZE * TILE_SIZE * bitsPerPixel / 8 : 0; }
};

struct TileView {
    const uint8_t *data = nullptr;
    Format format;

    explicit operator bool() const { return data && format.valid(); }
};

struct ContainerHeader {
    Format format;
    uint8_t layout = 0;
    uint8_t gridColumns = 0;
    uint8_t gridRows = 0;
    uint8_t blockCount = 0;
    uint32_t tileCount = 0;
    uint32_t dataSize = 0;
    uint32_t payloadOffset = 0;
    uint32_t dataOffset = 0;
};

struct PayloadDescriptor {
    uint8_t kind = 0;
    uint16_t compressedSize = 0;
    uint32_t dataOffset = 0;
};

bool parseMagic(const uint8_t *magic, Format &format);
bool parseHeader(const uint8_t *rawHeader, uint32_t fileSize, ContainerHeader &header);
bool parsePayload(const uint8_t *rawPayload, uint32_t dataSize, size_t compressedCapacity, PayloadDescriptor &descriptor);
uint8_t pixelAt(const TileView &tile, uint16_t x, uint16_t y);
size_t maxCompressedSize(Format format);

int decompress(const uint8_t *source, size_t sourceLength, uint8_t *destination, size_t destinationCapacity);

} // namespace NicheGraphics::InkHUD::MapTileUtils

#endif
