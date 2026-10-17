#ifdef MESHTASTIC_INCLUDE_INKHUD

#include "MapTileUtils.h"

#include <string.h>

namespace NicheGraphics::InkHUD::MapTileUtils
{
namespace
{

uint16_t readLittleEndian16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLittleEndian32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

} // namespace

bool parseMagic(const uint8_t *magic, Format &format)
{
    format = {};
    if (!magic || magic[0] != 'M' || magic[1] != 'T' || magic[2] != 'B')
        return false;

    switch (magic[3]) {
    case '1':
        format.bitsPerPixel = 1;
        break;
    case '2':
        format.bitsPerPixel = 2;
        break;
    case '4':
        format.bitsPerPixel = 4;
        break;
    default:
        return false;
    }
    return true;
}

bool parseHeader(const uint8_t *rawHeader, uint32_t fileSize, ContainerHeader &header)
{
    header = {};
    if (!rawHeader || fileSize < HEADER_SIZE || !parseMagic(rawHeader, header.format))
        return false;

    header.layout = rawHeader[4];
    header.gridColumns = rawHeader[5];
    header.gridRows = rawHeader[6];
    header.blockCount = rawHeader[7];
    header.tileCount = readLittleEndian32(rawHeader + 8);
    header.dataSize = readLittleEndian32(rawHeader + 12);

    const bool sparse = header.layout == LAYOUT_SPARSE;
    const bool grid = header.layout == LAYOUT_GRID && header.gridColumns && header.gridRows && header.blockCount &&
                      header.tileCount == static_cast<uint32_t>(header.gridColumns) * header.gridRows * header.blockCount;
    if (!header.tileCount || (!sparse && !grid))
        return false;

    const uint32_t positionCount = grid ? header.blockCount : header.tileCount;
    const uint64_t payloadOffset = HEADER_SIZE + static_cast<uint64_t>(positionCount) * POSITION_SIZE;
    const uint64_t dataOffset = payloadOffset + static_cast<uint64_t>(header.tileCount) * PAYLOAD_SIZE;
    const uint64_t expectedSize = dataOffset + header.dataSize;
    if (dataOffset > UINT32_MAX || expectedSize != fileSize)
        return false;

    header.payloadOffset = static_cast<uint32_t>(payloadOffset);
    header.dataOffset = static_cast<uint32_t>(dataOffset);
    return true;
}

bool parsePayload(const uint8_t *rawPayload, uint32_t dataSize, size_t compressedCapacity, PayloadDescriptor &descriptor)
{
    descriptor = {};
    if (!rawPayload)
        return false;

    descriptor.kind = rawPayload[0];
    descriptor.compressedSize = readLittleEndian16(rawPayload + 1);
    descriptor.dataOffset = readLittleEndian32(rawPayload + 3);
    if (descriptor.kind == KIND_WHITE || descriptor.kind == KIND_BLACK)
        return descriptor.compressedSize == 0 && descriptor.dataOffset == 0;

    return descriptor.kind == KIND_LZ4 && descriptor.compressedSize > 0 && descriptor.compressedSize <= compressedCapacity &&
           descriptor.dataOffset <= dataSize && descriptor.compressedSize <= dataSize - descriptor.dataOffset;
}

uint8_t pixelAt(const TileView &tile, uint16_t x, uint16_t y)
{
    if (!tile || x >= TILE_SIZE || y >= TILE_SIZE)
        return 0;

    const uint8_t pixelsPerByte = tile.format.pixelsPerByte();
    const size_t byteIndex = (x / pixelsPerByte) * TILE_SIZE + y;
    return Drivers::PackedPixels::readMsb(tile.data[byteIndex], x % pixelsPerByte, tile.format.bitsPerPixel);
}

size_t maxCompressedSize(Format format)
{
    const size_t tileBytes = format.tileBytes();
    return format.valid() ? tileBytes + tileBytes / 255 + 16 : 0;
}

int decompress(const uint8_t *source, size_t sourceLength, uint8_t *destination, size_t destinationCapacity)
{
    if (!source || !destination)
        return -1;

    const uint8_t *sourceEnd = source + sourceLength;
    uint8_t *destinationEnd = destination + destinationCapacity;
    uint8_t *output = destination;

    while (source < sourceEnd) {
        const uint8_t token = *source++;
        size_t literalLength = (token >> 4) & 0x0F;
        if (literalLength == 15) {
            uint8_t extension;
            do {
                if (source >= sourceEnd)
                    return -1;
                extension = *source++;
                literalLength += extension;
            } while (extension == 255);
        }

        if (literalLength > sourceEnd - source || literalLength > destinationEnd - output)
            return -1;
        memcpy(output, source, literalLength);
        output += literalLength;
        source += literalLength;
        if (source >= sourceEnd)
            break;
        if (sourceEnd - source < 2)
            return -1;

        const size_t offset = (size_t)source[0] | ((size_t)source[1] << 8);
        source += 2;
        if (offset == 0 || offset > static_cast<size_t>(output - destination))
            return -1;

        size_t matchLength = (token & 0x0F) + 4;
        if (matchLength == 19) {
            uint8_t extension;
            do {
                if (source >= sourceEnd)
                    return -1;
                extension = *source++;
                matchLength += extension;
            } while (extension == 255);
        }
        if (matchLength > destinationEnd - output)
            return -1;

        const uint8_t *match = output - offset;
        for (size_t i = 0; i < matchLength; ++i)
            *output++ = match[i];
    }

    return static_cast<int>(output - destination);
}

} // namespace NicheGraphics::InkHUD::MapTileUtils

#endif
