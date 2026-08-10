#pragma once

#if defined(T5_S3_EPAPER_PRO_V2)

#include "DebugConfiguration.h"
#include "FastEPD.h"
#include "variant.h"

#include <Arduino.h>

// FastEPD helper symbols are defined in FastEPD.inl with C++ linkage.
extern void bbepPCA9535DigitalWrite(uint8_t pin, uint8_t value);
extern uint8_t bbepPCA9535DigitalRead(uint8_t pin);
extern int bbepI2CWrite(unsigned char iAddr, unsigned char *pData, int iLen);
extern int bbepI2CReadRegister(unsigned char iAddr, unsigned char u8Register, unsigned char *pData, int iLen);

inline int t5S3V2EinkPower(void *pBBEP, int bOn)
{
    static bool warnedPgood = false;
    static bool warnedTpsPg = false;
    static bool warnedTpsWrite = false;

    FASTEPDSTATE *pState = static_cast<FASTEPDSTATE *>(pBBEP);
    if (!pState) {
        return BBEP_ERROR_BAD_PARAMETER;
    }

    if (bOn == pState->pwr_on) {
        return BBEP_SUCCESS;
    }

    if (bOn) {
        bbepPCA9535DigitalWrite(BOARD_PCA9535_EPD_OE, 1);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_EPD_MODE, 1);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_TPS_WAKEUP, 1);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_TPS_PWRUP, 1);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_EPD_VCOM_CTRL, 1);
        delay(1);

        const uint32_t pgoodStart = millis();
        bool pgoodSeen = false;
        while (!bbepPCA9535DigitalRead(BOARD_PCA9535_TPS_PWR_GOOD)) {
            if ((millis() - pgoodStart) > 1200) {
                if (!warnedPgood) {
                    LOG_WARN("ED047TC1: PWRGOOD timeout, continuing with fallback power-on path");
                    warnedPgood = true;
                }
                break;
            }
            delay(1);
        }
        if (bbepPCA9535DigitalRead(BOARD_PCA9535_TPS_PWR_GOOD)) {
            pgoodSeen = true;
        }

        uint8_t data[4] = {0};
        data[0] = 0x01;
        data[1] = 0x3f;
        const int tpsEnableRc = bbepI2CWrite(0x68, data, 2);

        const int vcom = pState->iVCOM / -10;
        data[0] = 3;
        data[1] = static_cast<uint8_t>(vcom);
        data[2] = static_cast<uint8_t>(vcom >> 8);
        const int tpsVcomRc = bbepI2CWrite(0x68, data, 3);
        if ((tpsEnableRc != 0 || tpsVcomRc != 0) && !warnedTpsWrite) {
            LOG_WARN("ED047TC1: TPS write did not ACK, continuing with fallback");
            warnedTpsWrite = true;
        }

        int timeout = 0;
        uint8_t value = 0;
        while (timeout < 400 && ((value & 0xfa) != 0xfa)) {
            bbepI2CReadRegister(0x68, 0x0F, &value, 1);
            timeout++;
            delay(1);
        }
        if (timeout >= 400 && !warnedTpsPg) {
            LOG_WARN(pgoodSeen ? "ED047TC1: TPS power-good register timeout, panel may still work"
                               : "ED047TC1: TPS power-good register timeout after PWRGOOD fallback");
            warnedTpsPg = true;
        }

        pState->pwr_on = 1;
    } else {
        bbepPCA9535DigitalWrite(BOARD_PCA9535_EPD_OE, 0);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_EPD_MODE, 0);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_TPS_PWRUP, 0);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_EPD_VCOM_CTRL, 0);
        delay(1);
        bbepPCA9535DigitalWrite(BOARD_PCA9535_TPS_WAKEUP, 0);
        pState->pwr_on = 0;
    }

    return BBEP_SUCCESS;
}

class T5S3V2SafeFastEPD : public FASTEPD
{
  public:
    void installSafePowerHandler() { _state.pfnEinkPower = t5S3V2EinkPower; }
};

#endif
