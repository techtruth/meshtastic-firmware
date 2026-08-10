// Pin-level early init only. All touch, backlight, and InkHUD code lives in
// src/platform/extra_variants/t5s3_epaper/variant.cpp where PlatformIO's
// library dependency finder can resolve headers like TouchDrvGT911.hpp.
#include "variant.h"
#include "Arduino.h"
#include "pins_arduino.h"

void earlyInitVariant()
{
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
    pinMode(SDCARD_CS, OUTPUT);
    digitalWrite(SDCARD_CS, HIGH);
    pinMode(BOARD_BL_EN, OUTPUT);
    // Backlight ON at boot (active-HIGH). Full backlight state management
    // lives in src/platform/extra_variants/t5s3_epaper/variant.cpp.
    digitalWrite(BOARD_BL_EN, HIGH);

    // Latch GT911 at 0x14 before the I2C scan; 0x5D collides with SFA30 detection.
    // touch.begin() repeats the full reset/init sequence later.
    pinMode(GT911_PIN_RST, OUTPUT);
    digitalWrite(GT911_PIN_RST, LOW);
    pinMode(GT911_PIN_INT, OUTPUT);
    digitalWrite(GT911_PIN_INT, HIGH); // latch address 0x14
    delay(1);                          // > 100 us
    digitalWrite(GT911_PIN_RST, HIGH);
    delay(10);                     // > 5 ms startup
    pinMode(GT911_PIN_INT, INPUT); // release INT for interrupt use
}
