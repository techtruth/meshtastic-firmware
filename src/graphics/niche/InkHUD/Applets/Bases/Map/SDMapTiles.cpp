#ifdef MESHTASTIC_INCLUDE_INKHUD

#include "SDMapTiles.h"

#include "MapTileUtils.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "graphics/niche/Drivers/EInk/EInk.h"

#if defined(INKHUD_USE_SDCARD_MAP_TILES)

#if !defined(HAS_SDCARD) || defined(SDCARD_USE_SOFT_SPI)
#error "INKHUD_USE_SDCARD_MAP_TILES requires hardware SPI SD card support"
#endif

#include "SPILock.h"
#include <Arduino.h>
#include <SD.h>
#include <stdlib.h>
#include <string.h>

#ifndef INKHUD_SD_MAP_INDEX_CACHE_PERCENT
#define INKHUD_SD_MAP_INDEX_CACHE_PERCENT 10U
#endif

namespace NicheGraphics::InkHUD::SDMapTiles
{
namespace
{

constexpr char MAP_PATH[] = "/Map/MapTile.bin";
constexpr uint32_t HEADER_RETRY_MS = 2000;
constexpr uint32_t STREAMED_INDEX_ENTRIES_PER_BLOCK = 64;
static_assert(INKHUD_SD_MAP_INDEX_CACHE_PERCENT <= 50, "SD map index cache percentage cannot exceed 50");

class SpiLockGuard
{
  public:
    explicit SpiLockGuard(uint32_t timeoutMs, bool acquire = true)
        : locked(acquire && spiLock->lock(timeoutMs)), usable(!acquire || locked)
    {
    }
    SpiLockGuard(const SpiLockGuard &) = delete;
    SpiLockGuard &operator=(const SpiLockGuard &) = delete;
    ~SpiLockGuard()
    {
        if (locked)
            spiLock->unlock();
    }
    explicit operator bool() const { return usable; }

  private:
    bool locked;
    bool usable;
};

struct Header : MapTileUtils::ContainerHeader {
    bool checked = false;
    bool valid = false;
    uint32_t zoomMask = 0;
};

Header header;
Drivers::EInk *display = nullptr;
uint8_t *indexData = nullptr;
uint32_t indexSize = 0;
uint32_t mapFileSize = 0;
uint32_t headerRetryAt = 0;
uint8_t *compressedTile = nullptr;
uint8_t *decodedTile = nullptr;
size_t compressedTileSize = 0;

uint16_t readLittleEndian16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

void releaseTileBuffers()
{
    free(compressedTile);
    free(decodedTile);
    compressedTile = nullptr;
    decodedTile = nullptr;
    compressedTileSize = 0;
}

bool allocateTileBuffers(MapTileUtils::Format format)
{
    releaseTileBuffers();
    const size_t decodedSize = format.tileBytes();
    compressedTileSize = MapTileUtils::maxCompressedSize(format);
#if defined(ARCH_ESP32)
    decodedTile = static_cast<uint8_t *>(ps_malloc(decodedSize));
    compressedTile = static_cast<uint8_t *>(ps_malloc(compressedTileSize));
#else
    decodedTile = static_cast<uint8_t *>(malloc(decodedSize));
    compressedTile = static_cast<uint8_t *>(malloc(compressedTileSize));
#endif
    if (decodedTile && compressedTile)
        return true;

    releaseTileBuffers();
    return false;
}

bool readCached(uint32_t offset, uint8_t *buffer, size_t size)
{
    if (indexData && offset <= indexSize && size <= indexSize - offset) {
        memcpy(buffer, indexData + offset, size);
        return true;
    }
    return false;
}

bool readSdBlock(uint32_t offset, uint8_t *buffer, size_t size, uint32_t *fileSize = nullptr)
{
    SpiLockGuard lock(250);
    if (!lock)
        return false;

    File file = SD.open(MAP_PATH, FILE_READ);
    if (!file)
        return false;
    const size_t currentFileSize = file.size();
    if (currentFileSize > UINT32_MAX) {
        file.close();
        return false;
    }
    if (fileSize)
        *fileSize = currentFileSize;
    const bool readOk =
        offset <= currentFileSize && size <= currentFileSize - offset && file.seek(offset) && file.read(buffer, size) == size;
    file.close();
    return readOk;
}

void deferHeaderRetry(const char *reason)
{
    headerRetryAt = Time::timerEndsAtMillis(HEADER_RETRY_MS);
    LOG_WARN("SD map: %s; retrying header", reason);
}

bool readPositionBlock(uint32_t firstEntry, uint32_t entryCount, uint8_t *buffer)
{
    const uint32_t offset = MapTileUtils::HEADER_SIZE + firstEntry * MapTileUtils::POSITION_SIZE;
    const size_t size = entryCount * MapTileUtils::POSITION_SIZE;
    if (readCached(offset, buffer, size))
        return true;
    const bool readOk = readSdBlock(offset, buffer, size);
    yield();
    return readOk;
}

bool scanZoomMask(Header &parsed)
{
    const uint32_t positionCount = parsed.layout == MapTileUtils::LAYOUT_GRID ? parsed.blockCount : parsed.tileCount;
    uint8_t positions[STREAMED_INDEX_ENTRIES_PER_BLOCK * MapTileUtils::POSITION_SIZE];
    for (uint32_t first = 0; first < positionCount; first += STREAMED_INDEX_ENTRIES_PER_BLOCK) {
        const uint32_t count = min(STREAMED_INDEX_ENTRIES_PER_BLOCK, positionCount - first);
        if (!readPositionBlock(first, count, positions))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t zoom = positions[i * MapTileUtils::POSITION_SIZE];
            if (zoom <= 22)
                parsed.zoomMask |= 1UL << zoom;
        }
    }
    return true;
}

int findTile(int zoom, int x, int y)
{
    const uint32_t positionCount = header.layout == MapTileUtils::LAYOUT_GRID ? header.blockCount : header.tileCount;
    const uint32_t tilesPerBlock = (uint32_t)header.gridColumns * header.gridRows;
    uint8_t positions[STREAMED_INDEX_ENTRIES_PER_BLOCK * MapTileUtils::POSITION_SIZE];
    for (uint32_t first = 0; first < positionCount; first += STREAMED_INDEX_ENTRIES_PER_BLOCK) {
        const uint32_t count = min(STREAMED_INDEX_ENTRIES_PER_BLOCK, positionCount - first);
        if (!readPositionBlock(first, count, positions)) {
            LOG_WARN("SD map: index block read failed at entry %lu", (unsigned long)first);
            return -1;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *position = positions + i * MapTileUtils::POSITION_SIZE;
            if (header.layout == MapTileUtils::LAYOUT_SPARSE) {
                if (position[0] == zoom && readLittleEndian16(position + 1) == x && readLittleEndian16(position + 3) == y)
                    return first + i;
                continue;
            }

            const int originX = readLittleEndian16(position + 1);
            const int originY = readLittleEndian16(position + 3);
            if (position[0] == zoom && x >= originX && x < originX + header.gridColumns && y >= originY &&
                y < originY + header.gridRows)
                return (first + i) * tilesPerBlock + (x - originX) * header.gridRows + (y - originY);
        }
    }
    return -1;
}

} // namespace

void preload(Drivers::EInk *driver)
{
    if (driver)
        display = driver;
    if (!display)
        return;
    if (header.checked || indexData)
        return;
    if (headerRetryAt && !Throttle::deadlinePassed(headerRetryAt))
        return;
    headerRetryAt = 0;

    uint8_t rawHeader[MapTileUtils::HEADER_SIZE];
    uint32_t fileSize = 0;
    if (!readSdBlock(0, rawHeader, sizeof(rawHeader), &fileSize)) {
        deferHeaderRetry("header read failed");
        return;
    }

    Header parsed;
    if (!MapTileUtils::parseHeader(rawHeader, fileSize, parsed)) {
        LOG_WARN("SD map: invalid tile index");
        parsed.checked = true;
        header = parsed;
        return;
    }

    if (!display->supportsPixelDepth(parsed.format.bitsPerPixel)) {
        LOG_WARN("SD map: MTB%u is not supported by this display", parsed.format.bitsPerPixel);
        parsed.checked = true;
        header = parsed;
        return;
    }
    if (!allocateTileBuffers(parsed.format)) {
        LOG_WARN("SD map: insufficient memory for MTB%u tile buffers", parsed.format.bitsPerPixel);
        parsed.checked = true;
        header = parsed;
        return;
    }
    const uint8_t previousPixelDepth = display->pixelDepth();
    if (!display->setPixelDepth(parsed.format.bitsPerPixel)) {
        LOG_WARN("SD map: cannot select MTB%u after display initialization", parsed.format.bitsPerPixel);
        releaseTileBuffers();
        parsed.checked = true;
        header = parsed;
        return;
    }

    uint8_t *candidateIndex = nullptr;
    uint32_t freePsram = 0;
    uint32_t totalPsram = 0;
    uint32_t cacheLimit = 0;
#if defined(ARCH_ESP32)
    freePsram = ESP.getFreePsram();
    totalPsram = ESP.getPsramSize();
    cacheLimit = ((uint64_t)freePsram * INKHUD_SD_MAP_INDEX_CACHE_PERCENT) / 100U;
    if (parsed.dataOffset <= cacheLimit)
        candidateIndex = static_cast<uint8_t *>(ps_malloc(parsed.dataOffset));
#endif

    if (candidateIndex) {
        constexpr size_t CACHE_READ_BLOCK_SIZE = 4096;
        bool cacheReadOk = true;
        for (uint32_t offset = 0; offset < parsed.dataOffset; offset += CACHE_READ_BLOCK_SIZE) {
            const size_t size = min((uint32_t)CACHE_READ_BLOCK_SIZE, parsed.dataOffset - offset);
            if (!readSdBlock(offset, candidateIndex + offset, size)) {
                cacheReadOk = false;
                break;
            }
            yield();
        }
        if (!cacheReadOk) {
            free(candidateIndex);
            releaseTileBuffers();
            deferHeaderRetry("index preload failed");
            return;
        }
        indexData = candidateIndex;
        indexSize = parsed.dataOffset;
        mapFileSize = fileSize;
        LOG_INFO("SD map: cached %lu-byte index for %lu-byte map (limit=%lu, PSRAM=%lu/%lu free)", (unsigned long)indexSize,
                 (unsigned long)mapFileSize, (unsigned long)cacheLimit, (unsigned long)(freePsram - indexSize),
                 (unsigned long)totalPsram);
    } else {
        LOG_INFO("SD map: streaming %lu-byte index (limit=%lu, PSRAM=%lu/%lu free)", (unsigned long)parsed.dataOffset,
                 (unsigned long)cacheLimit, (unsigned long)freePsram, (unsigned long)totalPsram);
    }

    if (!scanZoomMask(parsed)) {
        free(indexData);
        indexData = nullptr;
        indexSize = 0;
        mapFileSize = 0;
        releaseTileBuffers();
        deferHeaderRetry("position index read failed");
        return;
    }
    parsed.valid = parsed.tileCount > 0 && parsed.zoomMask != 0;
    if (!parsed.valid) {
        display->setPixelDepth(previousPixelDepth);
        free(indexData);
        indexData = nullptr;
        indexSize = 0;
        mapFileSize = 0;
        releaseTileBuffers();
        parsed.valid = false;
        parsed.checked = true;
        header = parsed;
        return;
    }
    parsed.checked = true;
    header = parsed;
    LOG_INFO("SD map: MTB%u ready (%lu-byte tiles)", header.format.bitsPerPixel, (unsigned long)header.format.tileBytes());
}

bool hasZoom(int zoom)
{
    preload();
    return header.valid && zoom >= 0 && zoom <= 22 && (header.zoomMask & (1UL << zoom));
}

MapTileUtils::Format format()
{
    preload();
    return header.valid ? header.format : MapTileUtils::Format{};
}

MapTileUtils::TileView readTile(int zoom, int x, int y)
{
    if (!decodedTile || !compressedTile || !hasZoom(zoom))
        return {};

    const int tileIndex = findTile(zoom, x, y);
    if (tileIndex < 0)
        return {};

    uint8_t payload[MapTileUtils::PAYLOAD_SIZE];
    const uint32_t payloadOffset = header.payloadOffset + tileIndex * MapTileUtils::PAYLOAD_SIZE;
    if (!readCached(payloadOffset, payload, sizeof(payload)) && !readSdBlock(payloadOffset, payload, sizeof(payload)))
        return {};

    MapTileUtils::PayloadDescriptor descriptor;
    if (!MapTileUtils::parsePayload(payload, header.dataSize, compressedTileSize, descriptor))
        return {};
    if (descriptor.kind == MapTileUtils::KIND_WHITE || descriptor.kind == MapTileUtils::KIND_BLACK) {
        memset(decodedTile, descriptor.kind == MapTileUtils::KIND_WHITE ? 0xFF : 0x00, header.format.tileBytes());
        return {decodedTile, header.format};
    }

    if (!readSdBlock(header.dataOffset + descriptor.dataOffset, compressedTile, descriptor.compressedSize)) {
        LOG_WARN("SD map: tile data read failed z%d/%d/%d", zoom, x, y);
        return {};
    }

    const int decoded =
        MapTileUtils::decompress(compressedTile, descriptor.compressedSize, decodedTile, header.format.tileBytes());
    return decoded == static_cast<int>(header.format.tileBytes()) ? MapTileUtils::TileView{decodedTile, header.format}
                                                                  : MapTileUtils::TileView{};
}

} // namespace NicheGraphics::InkHUD::SDMapTiles

#else

namespace NicheGraphics::InkHUD::SDMapTiles
{

void preload(Drivers::EInk *driver)
{
    (void)driver;
}
bool hasZoom(int zoom)
{
    (void)zoom;
    return false;
}
MapTileUtils::Format format()
{
    return {};
}
MapTileUtils::TileView readTile(int zoom, int x, int y)
{
    (void)zoom;
    (void)x;
    (void)y;
    return {};
}

} // namespace NicheGraphics::InkHUD::SDMapTiles

#endif
#endif
