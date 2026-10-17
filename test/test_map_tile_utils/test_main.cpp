// Unit tests for the native-depth MTB format and packed-framebuffer helpers in
// MapTileUtils.{h,cpp} and PackedPixels.h.
//
// MTB1, MTB2 and MTB4 must remain distinct native formats: the magic selects the
// decoded size and packed-pixel width, and pixelAt() must return the stored value
// unchanged. These tests guard against accepting the transposed MBT spelling,
// reversing field order, or introducing implicit depth conversion.
#include "TestUtil.h"
#include <cstring>
#include <unity.h>
#include <vector>

#ifndef MESHTASTIC_INCLUDE_INKHUD
#define MESHTASTIC_INCLUDE_INKHUD
#endif
#include "../../src/graphics/niche/InkHUD/Applets/Bases/Map/MapTileUtils.cpp"

using namespace NicheGraphics::InkHUD::MapTileUtils;

void setUp(void) {}
void tearDown(void) {}

static void writeLittleEndian32(uint8_t *destination, uint32_t value)
{
    destination[0] = value;
    destination[1] = value >> 8;
    destination[2] = value >> 16;
    destination[3] = value >> 24;
}

static void writeLittleEndian16(uint8_t *destination, uint16_t value)
{
    destination[0] = value;
    destination[1] = value >> 8;
}

static void makeHeader(uint8_t *header, const char *magic, uint8_t layout, uint8_t columns, uint8_t rows, uint8_t blocks,
                       uint32_t tiles, uint32_t dataSize)
{
    memcpy(header, magic, 4);
    header[4] = layout;
    header[5] = columns;
    header[6] = rows;
    header[7] = blocks;
    writeLittleEndian32(header + 8, tiles);
    writeLittleEndian32(header + 12, dataSize);
}

void test_parseMagic_accepts_native_depths(void)
{
    Format format;

    TEST_ASSERT_TRUE(parseMagic(reinterpret_cast<const uint8_t *>("MTB1"), format));
    TEST_ASSERT_EQUAL_UINT8(1, format.bitsPerPixel);
    TEST_ASSERT_EQUAL_UINT32(8192, format.tileBytes());
    TEST_ASSERT_GREATER_THAN_UINT32(format.tileBytes(), maxCompressedSize(format));

    TEST_ASSERT_TRUE(parseMagic(reinterpret_cast<const uint8_t *>("MTB2"), format));
    TEST_ASSERT_EQUAL_UINT8(2, format.bitsPerPixel);
    TEST_ASSERT_EQUAL_UINT32(16384, format.tileBytes());
    TEST_ASSERT_GREATER_THAN_UINT32(format.tileBytes(), maxCompressedSize(format));

    TEST_ASSERT_TRUE(parseMagic(reinterpret_cast<const uint8_t *>("MTB4"), format));
    TEST_ASSERT_EQUAL_UINT8(4, format.bitsPerPixel);
    TEST_ASSERT_EQUAL_UINT32(32768, format.tileBytes());
    TEST_ASSERT_GREATER_THAN_UINT32(format.tileBytes(), maxCompressedSize(format));
}

void test_parseMagic_rejects_legacy_and_unknown_magic(void)
{
    Format format{4};

    TEST_ASSERT_FALSE(parseMagic(reinterpret_cast<const uint8_t *>("MBT1"), format));
    TEST_ASSERT_FALSE(format.valid());
    TEST_ASSERT_FALSE(parseMagic(reinterpret_cast<const uint8_t *>("MTB3"), format));
    TEST_ASSERT_FALSE(parseMagic(nullptr, format));
}

void test_parseHeader_validates_sparse_and_grid_layouts(void)
{
    uint8_t raw[HEADER_SIZE];
    ContainerHeader header;

    makeHeader(raw, "MTB1", LAYOUT_SPARSE, 0, 0, 0, 1, 0);
    TEST_ASSERT_TRUE(parseHeader(raw, HEADER_SIZE + POSITION_SIZE + PAYLOAD_SIZE, header));
    TEST_ASSERT_EQUAL_UINT32(HEADER_SIZE + POSITION_SIZE, header.payloadOffset);
    TEST_ASSERT_EQUAL_UINT32(HEADER_SIZE + POSITION_SIZE + PAYLOAD_SIZE, header.dataOffset);

    makeHeader(raw, "MTB4", LAYOUT_GRID, 2, 3, 1, 6, 10);
    const uint32_t gridSize = HEADER_SIZE + POSITION_SIZE + 6 * PAYLOAD_SIZE + 10;
    TEST_ASSERT_TRUE(parseHeader(raw, gridSize, header));
    TEST_ASSERT_EQUAL_UINT8(4, header.format.bitsPerPixel);

    makeHeader(raw, "MTB2", LAYOUT_SPARSE, 0, 0, 0, 1, 0);
    TEST_ASSERT_TRUE(parseHeader(raw, HEADER_SIZE + POSITION_SIZE + PAYLOAD_SIZE, header));
    TEST_ASSERT_EQUAL_UINT8(2, header.format.bitsPerPixel);
}

void test_parseHeader_rejects_inconsistent_sizes_and_counts(void)
{
    uint8_t raw[HEADER_SIZE];
    ContainerHeader header;

    makeHeader(raw, "MTB2", LAYOUT_SPARSE, 0, 0, 0, 0, 0);
    TEST_ASSERT_FALSE(parseHeader(raw, HEADER_SIZE, header));

    makeHeader(raw, "MTB2", LAYOUT_GRID, 2, 3, 1, 5, 0);
    TEST_ASSERT_FALSE(parseHeader(raw, HEADER_SIZE + POSITION_SIZE + 5 * PAYLOAD_SIZE, header));

    makeHeader(raw, "MTB2", 2, 0, 0, 0, 1, 0);
    TEST_ASSERT_FALSE(parseHeader(raw, HEADER_SIZE + POSITION_SIZE + PAYLOAD_SIZE, header));

    makeHeader(raw, "MTB2", LAYOUT_SPARSE, 0, 0, 0, UINT32_MAX, 0);
    TEST_ASSERT_FALSE(parseHeader(raw, UINT32_MAX, header));

    makeHeader(raw, "MTB2", LAYOUT_SPARSE, 0, 0, 0, 1, 0);
    const uint32_t exactSize = HEADER_SIZE + POSITION_SIZE + PAYLOAD_SIZE;
    TEST_ASSERT_FALSE(parseHeader(raw, exactSize - 1, header));
    TEST_ASSERT_FALSE(parseHeader(raw, exactSize + 1, header));
}

void test_pixelAt_reads_msb_first_without_depth_conversion(void)
{
    static uint8_t tile[MAX_TILE_BYTES] = {};

    tile[0] = 0x81;
    tile[1] = 0x40;
    TEST_ASSERT_EQUAL_UINT8(1, pixelAt({tile, Format{1}}, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(1, pixelAt({tile, Format{1}}, 7, 0));
    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({tile, Format{1}}, 8, 0));
    TEST_ASSERT_EQUAL_UINT8(1, pixelAt({tile, Format{1}}, 9, 0));

    tile[0] = 0x1B;
    tile[1] = 0xC0;
    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({tile, Format{2}}, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(1, pixelAt({tile, Format{2}}, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(2, pixelAt({tile, Format{2}}, 2, 0));
    TEST_ASSERT_EQUAL_UINT8(3, pixelAt({tile, Format{2}}, 3, 0));
    TEST_ASSERT_EQUAL_UINT8(3, pixelAt({tile, Format{2}}, 4, 0));
    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({tile, Format{2}}, 5, 0));

    tile[0] = 0xA5;
    tile[1] = 0x3C;
    TEST_ASSERT_EQUAL_UINT8(10, pixelAt({tile, Format{4}}, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(5, pixelAt({tile, Format{4}}, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(3, pixelAt({tile, Format{4}}, 2, 0));
    TEST_ASSERT_EQUAL_UINT8(12, pixelAt({tile, Format{4}}, 3, 0));
}

void test_packedPixels_writes_msb_first_at_every_depth(void)
{
    uint8_t packed = 0;
    for (uint8_t pixel = 0; pixel < 8; ++pixel)
        NicheGraphics::Drivers::PackedPixels::writeMsb(packed, pixel, 1, pixel & 1);
    TEST_ASSERT_EQUAL_HEX8(0x55, packed);

    packed = 0;
    for (uint8_t pixel = 0; pixel < 4; ++pixel)
        NicheGraphics::Drivers::PackedPixels::writeMsb(packed, pixel, 2, pixel);
    TEST_ASSERT_EQUAL_HEX8(0x1B, packed);

    packed = 0;
    NicheGraphics::Drivers::PackedPixels::writeMsb(packed, 0, 4, 10);
    NicheGraphics::Drivers::PackedPixels::writeMsb(packed, 1, 4, 5);
    TEST_ASSERT_EQUAL_HEX8(0xA5, packed);

    packed = 0;
    NicheGraphics::Drivers::PackedPixels::writeMsb(packed, 0, 2, 15);
    TEST_ASSERT_EQUAL_HEX8(0xC0, packed);
}

void test_packedPixels_preserves_endpoints_and_row_sizes(void)
{
    const uint8_t depths[] = {1, 2, 4};
    for (const uint8_t depth : depths) {
        const uint8_t maximum = NicheGraphics::Drivers::PackedPixels::maxValue(depth);
        const uint8_t pixels = NicheGraphics::Drivers::PackedPixels::pixelsPerByte(depth);
        for (uint8_t pixel = 0; pixel < pixels; ++pixel) {
            TEST_ASSERT_EQUAL_UINT8(maximum, NicheGraphics::Drivers::PackedPixels::readMsb(0xFF, pixel, depth));
            TEST_ASSERT_EQUAL_UINT8(0, NicheGraphics::Drivers::PackedPixels::readMsb(0x00, pixel, depth));
        }
        TEST_ASSERT_EQUAL_UINT8(0, NicheGraphics::Drivers::PackedPixels::readMsb(0xFF, pixels, depth));
    }

    TEST_ASSERT_EQUAL_UINT32(2, NicheGraphics::Drivers::PackedPixels::rowBytes(9, 1));
    TEST_ASSERT_EQUAL_UINT32(3, NicheGraphics::Drivers::PackedPixels::rowBytes(9, 2));
    TEST_ASSERT_EQUAL_UINT32(5, NicheGraphics::Drivers::PackedPixels::rowBytes(9, 4));
    TEST_ASSERT_EQUAL_UINT32(0, NicheGraphics::Drivers::PackedPixels::rowBytes(9, 3));

    const uint8_t inverted = static_cast<uint8_t>(~0x1B);
    TEST_ASSERT_EQUAL_UINT8(3, NicheGraphics::Drivers::PackedPixels::readMsb(inverted, 0, 2));
    TEST_ASSERT_EQUAL_UINT8(2, NicheGraphics::Drivers::PackedPixels::readMsb(inverted, 1, 2));
    TEST_ASSERT_EQUAL_UINT8(1, NicheGraphics::Drivers::PackedPixels::readMsb(inverted, 2, 2));
    TEST_ASSERT_EQUAL_UINT8(0, NicheGraphics::Drivers::PackedPixels::readMsb(inverted, 3, 2));
}

void test_packedPixels_fills_aligned_and_unaligned_spans(void)
{
    uint8_t mono[3] = {};
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(mono, 3, 18, 1, 1);
    TEST_ASSERT_EQUAL_HEX8(0x1F, mono[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, mono[1]);
    TEST_ASSERT_EQUAL_HEX8(0xC0, mono[2]);

    uint8_t gray2[3] = {};
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(gray2, 1, 11, 2, 3);
    TEST_ASSERT_EQUAL_HEX8(0x3F, gray2[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, gray2[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFC, gray2[2]);

    uint8_t gray4[3] = {};
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(gray4, 1, 5, 4, 15);
    TEST_ASSERT_EQUAL_HEX8(0x0F, gray4[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, gray4[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, gray4[2]);

    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(gray4, 2, 4, 4, 0);
    TEST_ASSERT_EQUAL_HEX8(0x0F, gray4[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, gray4[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, gray4[2]);

    uint8_t intermediate[2] = {};
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(intermediate, 0, 4, 2, 2);
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(intermediate + 1, 0, 2, 4, 10);
    TEST_ASSERT_EQUAL_HEX8(0xAA, intermediate[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, intermediate[1]);

    uint8_t unchanged = 0x5A;
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(&unchanged, 4, 4, 2, 3);
    TEST_ASSERT_EQUAL_HEX8(0x5A, unchanged);
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(&unchanged, 0, 4, 3, 7);
    TEST_ASSERT_EQUAL_HEX8(0x5A, unchanged);
    NicheGraphics::Drivers::PackedPixels::fillSpanMsb(nullptr, 0, 4, 2, 3);
}

void test_packedPixels_capability_masks_require_native_depth(void)
{
    const uint8_t monochrome = 1;
    TEST_ASSERT_TRUE(NicheGraphics::Drivers::PackedPixels::supports(monochrome, 1));
    TEST_ASSERT_FALSE(NicheGraphics::Drivers::PackedPixels::supports(monochrome, 2));
    TEST_ASSERT_FALSE(NicheGraphics::Drivers::PackedPixels::supports(monochrome, 4));

    const uint8_t t5s3 = 1 | 4;
    TEST_ASSERT_TRUE(NicheGraphics::Drivers::PackedPixels::supports(t5s3, 1));
    TEST_ASSERT_FALSE(NicheGraphics::Drivers::PackedPixels::supports(t5s3, 2));
    TEST_ASSERT_TRUE(NicheGraphics::Drivers::PackedPixels::supports(t5s3, 4));
    TEST_ASSERT_FALSE(NicheGraphics::Drivers::PackedPixels::supports(t5s3, 3));
}

void test_pixelAt_preserves_black_and_white_at_every_depth(void)
{
    static uint8_t black[MAX_TILE_BYTES] = {};
    static uint8_t white[MAX_TILE_BYTES];
    memset(white, 0xFF, sizeof(white));

    const uint8_t depths[] = {1, 2, 4};
    for (const uint8_t depth : depths) {
        const Format format{depth};
        TEST_ASSERT_EQUAL_UINT8(0, pixelAt({black, format}, 0, 0));
        TEST_ASSERT_EQUAL_UINT8(0, pixelAt({black, format}, TILE_SIZE - 1, TILE_SIZE - 1));
        TEST_ASSERT_EQUAL_UINT8(format.maxValue(), pixelAt({white, format}, 0, 0));
        TEST_ASSERT_EQUAL_UINT8(format.maxValue(), pixelAt({white, format}, TILE_SIZE - 1, TILE_SIZE - 1));
    }
}

void test_parsePayload_validates_solid_and_lz4_descriptors(void)
{
    uint8_t raw[PAYLOAD_SIZE] = {};
    PayloadDescriptor descriptor;

    raw[0] = KIND_WHITE;
    TEST_ASSERT_TRUE(parsePayload(raw, 0, 0, descriptor));
    raw[3] = 1;
    TEST_ASSERT_FALSE(parsePayload(raw, 1, 0, descriptor));

    memset(raw, 0, sizeof(raw));
    raw[0] = KIND_BLACK;
    TEST_ASSERT_TRUE(parsePayload(raw, 0, 0, descriptor));
    raw[1] = 1;
    TEST_ASSERT_FALSE(parsePayload(raw, 1, 1, descriptor));

    memset(raw, 0, sizeof(raw));
    raw[0] = KIND_LZ4;
    writeLittleEndian16(raw + 1, 4);
    writeLittleEndian32(raw + 3, 6);
    TEST_ASSERT_TRUE(parsePayload(raw, 10, 4, descriptor));
    TEST_ASSERT_EQUAL_UINT16(4, descriptor.compressedSize);
    TEST_ASSERT_EQUAL_UINT32(6, descriptor.dataOffset);
    TEST_ASSERT_FALSE(parsePayload(raw, 9, 4, descriptor));
    TEST_ASSERT_FALSE(parsePayload(raw, 10, 3, descriptor));
}

void test_pixelAt_rejects_invalid_views_and_coordinates(void)
{
    const uint8_t tile[] = {0xFF};

    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({}, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({tile, Format{3}}, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({tile, Format{1}}, TILE_SIZE, 0));
    TEST_ASSERT_EQUAL_UINT8(0, pixelAt({tile, Format{1}}, 0, TILE_SIZE));
}

void test_decompress_accepts_literals_and_rejects_invalid_match(void)
{
    const uint8_t literal[] = {0x30, 0x11, 0x22, 0x33};
    uint8_t output[4] = {};
    TEST_ASSERT_EQUAL_INT(3, decompress(literal, sizeof(literal), output, sizeof(output)));
    TEST_ASSERT_EQUAL_HEX8(0x11, output[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, output[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, output[2]);
    TEST_ASSERT_EQUAL_INT(-1, decompress(literal, sizeof(literal), output, 2));

    const uint8_t invalidMatch[] = {0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(-1, decompress(invalidMatch, sizeof(invalidMatch), output, sizeof(output)));
}

void test_decompress_requires_exact_native_tile_capacity(void)
{
    static uint8_t output[MAX_TILE_BYTES];
    const uint8_t depths[] = {1, 2, 4};

    for (const uint8_t depth : depths) {
        const size_t decodedSize = Format{depth}.tileBytes();
        size_t extension = decodedSize - 20;
        std::vector<uint8_t> compressed = {0x1F, 0xA5, 0x01, 0x00};
        while (extension >= 255) {
            compressed.push_back(255);
            extension -= 255;
        }
        compressed.push_back(static_cast<uint8_t>(extension));

        TEST_ASSERT_EQUAL_INT(static_cast<int>(decodedSize),
                              decompress(compressed.data(), compressed.size(), output, decodedSize));
        TEST_ASSERT_EQUAL_HEX8(0xA5, output[0]);
        TEST_ASSERT_EQUAL_HEX8(0xA5, output[decodedSize - 1]);
        TEST_ASSERT_EQUAL_INT(-1, decompress(compressed.data(), compressed.size(), output, decodedSize - 1));
    }
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    RUN_TEST(test_parseMagic_accepts_native_depths);
    RUN_TEST(test_parseMagic_rejects_legacy_and_unknown_magic);
    RUN_TEST(test_parseHeader_validates_sparse_and_grid_layouts);
    RUN_TEST(test_parseHeader_rejects_inconsistent_sizes_and_counts);
    RUN_TEST(test_parsePayload_validates_solid_and_lz4_descriptors);
    RUN_TEST(test_pixelAt_reads_msb_first_without_depth_conversion);
    RUN_TEST(test_packedPixels_writes_msb_first_at_every_depth);
    RUN_TEST(test_packedPixels_preserves_endpoints_and_row_sizes);
    RUN_TEST(test_packedPixels_fills_aligned_and_unaligned_spans);
    RUN_TEST(test_packedPixels_capability_masks_require_native_depth);
    RUN_TEST(test_pixelAt_preserves_black_and_white_at_every_depth);
    RUN_TEST(test_pixelAt_rejects_invalid_views_and_coordinates);
    RUN_TEST(test_decompress_accepts_literals_and_rejects_invalid_match);
    RUN_TEST(test_decompress_requires_exact_native_tile_capacity);

    exit(UNITY_END());
}

void loop() {}
