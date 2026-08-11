/*

    NicheGraphics parallel E-Ink driver for the LilyGo T5-S3-ePaper-Pro (ED047TC1).

    InkHUD buffer format : 1bpp, horizontal bytes, MSB = leftmost pixel, 1 = white
    FastEPD buffer format: 1bpp, horizontal bytes, MSB = leftmost pixel, 1 = white

    Both formats share the same pixel layout and polarity (1 = white, 0 = black).
    The InkHUD safe-area buffer (928×508) is copied into the centre of the physical
    960×540 FastEPD buffer so content clears the panel's inactive edge border.
    See ED047TC1.h for the H_OFFSET_BYTES / V_OFFSET_TOP / V_OFFSET_BOTTOM constants.

*/

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
#ifdef T5_S3_EPAPER_PRO

#include "./ED047TC1.h"

#include "FastEPD.h"
#include "configuration.h"
#include "graphics/eink/T5S3V2EpdPower.h"

using namespace NicheGraphics::Drivers;

namespace
{
#if defined(T5_S3_EPAPER_PRO_V2)
using SafeFastEPD = T5S3V2SafeFastEPD;
#else
class SafeFastEPD : public FASTEPD
{
  public:
    void installSafePowerHandler() {}
};
#endif
} // namespace

void ED047TC1::begin(SPIClass *spi, uint8_t pin_dc, uint8_t pin_cs, uint8_t pin_busy, uint8_t pin_rst)
{
    // Parallel display - SPI parameters are not used
    (void)spi;
    (void)pin_dc;
    (void)pin_cs;
    (void)pin_busy;
    (void)pin_rst;

    SafeFastEPD *safeEpaper = new SafeFastEPD;
    epaper = safeEpaper;

    int initRc = BBEP_ERROR_BAD_PARAMETER;
#if defined(T5_S3_EPAPER_PRO_V1)
    initRc = epaper->initPanel(BB_PANEL_LILYGO_T5PRO, 28000000);
#elif defined(T5_S3_EPAPER_PRO_V2)
    initRc = epaper->initPanel(BB_PANEL_LILYGO_T5PRO_V2, 28000000);
    // On this board, the physical side key is labeled IO48; electrically it maps to PCA9535 IO12.
    // FastEPD's generic V7 init drives 8..13 as outputs; force IO12 back to input
    // so variant touch-control polling can read the key reliably.
    epaper->ioPinMode(BOARD_PCA9535_BUTTON, INPUT);
#else
#error "ED047TC1 driver: unsupported variant - define T5_S3_EPAPER_PRO_V1 or T5_S3_EPAPER_PRO_V2"
#endif

    if (initRc != BBEP_SUCCESS || epaper->currentBuffer() == nullptr) {
        LOG_ERROR("ED047TC1 initPanel failed rc=%d; running headless", initRc);
        delete epaper;
        epaper = nullptr;
        return;
    }

    safeEpaper->installSafePowerHandler();

    const int modeRc = epaper->setMode(BB_MODE_1BPP);
    if (modeRc != BBEP_SUCCESS) {
        LOG_WARN("ED047TC1 setMode failed rc=%d", modeRc);
    }

    const int clearRc = epaper->clearWhite();
    if (clearRc != BBEP_SUCCESS) {
        LOG_WARN("ED047TC1 clearWhite failed rc=%d", clearRc);
    }

    const int fullRc = epaper->fullUpdate(true); // Blocking initial clear
    if (fullRc != BBEP_SUCCESS) {
        LOG_WARN("ED047TC1 initial fullUpdate failed rc=%d", fullRc);
    }
}

void ED047TC1::update(uint8_t *imageData, UpdateTypes type)
{
    if (!epaper)
        return;

    // InkHUD renders into a DISPLAY_WIDTH × DISPLAY_HEIGHT safe-area buffer.
    // We need to place that into the centre of the physical 960×540 FastEPD buffer,
    // leaving blank margins at every edge to avoid the panel's inactive border.
    const uint32_t srcRowBytes = (DISPLAY_WIDTH + 7) / 8; // bytes per row in InkHUD buffer (116)
    const uint32_t dstRowBytes = (960 + 7) / 8;           // bytes per row in physical buffer (120)
    const uint32_t dstTotalRows = 540;

    uint8_t *cur = epaper->currentBuffer();
    if (cur == nullptr) {
        LOG_ERROR("ED047TC1 framebuffer unavailable; skipping update");
        return;
    }

    // Fill physical buffer with white (0xFF = white in FastEPD 1bpp)
    memset(cur, 0xFF, dstRowBytes * dstTotalRows);

    // Copy each InkHUD row into the physical buffer with horizontal + vertical offsets
    for (uint32_t row = 0; row < DISPLAY_HEIGHT; row++) {
        const uint8_t *srcRow = imageData + row * srcRowBytes;
        uint8_t *dstRow = cur + (row + V_OFFSET_TOP) * dstRowBytes + H_OFFSET_BYTES;
        memcpy(dstRow, srcRow, srcRowBytes);
    }

    if (type == FULL) {
        epaper->fullUpdate(CLEAR_SLOW, false);
        epaper->backupPlane(); // Sync pPrevious so next partialUpdate has a correct baseline
    } else {
        // FAST: true partial update - compares pCurrent vs pPrevious and only applies
        // update waveform to rows that changed. partialUpdate() updates pPrevious.
        epaper->partialUpdate(false, 0, dstTotalRows - 1);
    }
}

#endif // T5_S3_EPAPER_PRO
#endif // MESHTASTIC_INCLUDE_NICHE_GRAPHICS
