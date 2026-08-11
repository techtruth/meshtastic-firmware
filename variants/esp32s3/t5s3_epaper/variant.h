#include "pins_arduino.h"

// Display (E-Ink) ED047TC1 - 8bit parallel
#define EPD_WIDTH 960
#define EPD_HEIGHT 540

#define CANNED_MESSAGE_MODULE_ENABLE 1
#define USE_VIRTUAL_KEYBOARD 1

// Board revision names:
// - H752: original T5 ePaper S3 Pro, no GPS/TPS65185/PCA9535.
// - H752-01 / H752-02: V2 hardware with PCA9535, TPS65185, GPS, and SX1262.
#if defined(T5_S3_EPAPER_PRO_V1)
#define BOARD_BL_EN 40
#else
#define BOARD_BL_EN 11
#endif

#define I2C_SDA SDA
#define I2C_SCL SCL

#define HAS_TOUCHSCREEN 1
#define TOUCH_POLL_INTERVAL_IDLE 25
#define TOUCH_POLL_INTERVAL_ACTIVE 15
#define TOUCH_POLL_INTERVAL_RELEASE 20
#define TOUCH_POLL_INTERVAL_ACTIVE_FAST 8
#define TOUCH_POLL_INTERVAL_RELEASE_FAST 8
#define GT911_PIN_SDA SDA
#define GT911_PIN_SCL SCL
#if defined(T5_S3_EPAPER_PRO_V1)
#define GT911_PIN_INT 15
#define GT911_PIN_RST 41
#else
#define GT911_PIN_INT 3
#define GT911_PIN_RST 9
#endif
// Do not use touch as a light-sleep wake source on T5-S3.
// Wake should come from physical buttons/radio/timer only.
#define SCREEN_TOUCH_INT GT911_PIN_INT

// Touch control helpers for this variant
bool isTouchInputEnabled();
void setTouchInputEnabled(bool enabled, bool showIndicator);
void toggleTouchInputEnabled();

// Backlight control helpers for this variant (non-latching behavior)
void t5BacklightSetUserEnabled(bool enabled);
bool t5BacklightIsUserEnabled();
void t5BacklightToggleUser();
void t5BacklightSetForcedByTimeout(bool forced);
void t5BacklightSetForcedBySleep(bool forced);
void t5BacklightHandleUserInput();

// Touch timeout/wake helpers for this variant
void t5TouchSetForcedByTimeout(bool forced);
bool t5TouchIsForcedByTimeout();
void t5TouchHandleUserInput();

// Gate GT911 capacitive-home callback delivery until InkHUD startup is complete.
void t5SetHomeCapButtonEventsEnabled(bool enabled);

#define PCF8563_RTC 0x51
#define HAS_RTC 1
#define PCF8563_INT 2

#define USE_POWERSAVE
#define SLEEP_TIME 120

// GPS
#if !defined(T5_S3_EPAPER_PRO_V1)
#define GPS_RX_PIN 44
#define GPS_TX_PIN 43
#endif

#if defined(T5_S3_EPAPER_PRO_V1)
#define BUTTON_PIN 48
#define PIN_BUTTON2 0
#define ALT_BUTTON_PIN PIN_BUTTON2
#else
#define BUTTON_PIN 0
#define BOARD_PCA9535_ADDR 0x20
#define BOARD_PCA9535_INT 38

// PCA9535 pin numbering follows FastEPD: port 0 is pins 0..7, port 1 is pins 8..15.
// H752-01/H752-02 map IO0_0 to LORA_EN, a shared 3.3V rail for SX1262 + GPS.
#define BOARD_PCA9535_LORA_GPS_EN 0
#define BOARD_PCA9535_IO0_1_NC 1
#define BOARD_PCA9535_IO0_2_NC 2
#define BOARD_PCA9535_IO0_3_NC 3
#define BOARD_PCA9535_IO0_4_NC 4
#define BOARD_PCA9535_IO0_5_NC 5
#define BOARD_PCA9535_IO0_6_NC 6
#define BOARD_PCA9535_IO0_7_NC 7
#define BOARD_PCA9535_EPD_OE 8
#define BOARD_PCA9535_EPD_MODE 9
// LilyGO labels this physical key as IO48; ESP32 GPIO48 is actually EPD CKV.
#define BOARD_PCA9535_BUTTON 10
#define BOARD_PCA9535_TPS_PWRUP 11
#define BOARD_PCA9535_EPD_VCOM_CTRL 12
#define BOARD_PCA9535_TPS_WAKEUP 13
#define BOARD_PCA9535_TPS_PWR_GOOD 14
#define BOARD_PCA9535_TPS_INT 15

#define BOARD_PCA9535_PORT0_POLARITY 0x00
#define BOARD_PCA9535_PORT1_POLARITY 0x00
#define BOARD_PCA9535_PORT0_CONFIG 0x00
#define BOARD_PCA9535_PORT1_CONFIG 0xC4 // IO1_2 IO48 key, IO1_6 PWRGOOD, IO1_7 TPS_INT are inputs.
#define BOARD_PCA9535_PORT0_OUTPUT_NORMAL 0xFF
#define BOARD_PCA9535_PORT0_OUTPUT_SLEEP 0xFE
#define BOARD_PCA9535_PORT1_OUTPUT_EPD_OFF 0x00
#define BOARD_PCA9535_PORT1_OUTPUT_EPD_ACTIVE 0x3B
#define BOARD_PCA9535_PORT1_OUTPUT_SLEEP BOARD_PCA9535_PORT1_OUTPUT_EPD_OFF
#define BOARD_PCA9535_PORT0_OUTPUT_BOOT BOARD_PCA9535_PORT0_OUTPUT_NORMAL
#define BOARD_PCA9535_PORT1_OUTPUT_BOOT BOARD_PCA9535_PORT1_OUTPUT_EPD_OFF
#define BOARD_PCA9535_PORT0_CONFIG_BOOT BOARD_PCA9535_PORT0_CONFIG
#define BOARD_PCA9535_PORT1_CONFIG_BOOT BOARD_PCA9535_PORT1_CONFIG
#define BOARD_PCA9535_BUTTON_MASK 0x04
#endif
// SD card
#define HAS_SDCARD
#define SDCARD_CS SPI_CS
#define SD_SPI_FREQUENCY 75000000U

// battery charger BQ25896
#define HAS_PPM 1
#define XPOWERS_CHIP_BQ25896
#if !defined(T5_S3_EPAPER_PRO_V1)
#define BQ25896_SYS_POWER_DOWN_MV 3300
#define BQ25896_INPUT_CURRENT_LIMIT_MA 3250
#define BQ25896_DISABLE_CURRENT_LIMIT_PIN 1
#define BQ25896_CHARGE_TARGET_MV 4208
#define BQ25896_PRECHARGE_CURRENT_MA 64
#define BQ25896_CHARGE_CURRENT_MA 1024
#define BQ25896_CYCLE_OTG_AFTER_INIT 1
#endif

// battery quality management BQ27220
#define HAS_BQ27220 1
#define BQ27220_I2C_SDA SDA
#define BQ27220_I2C_SCL SCL
#define BQ27220_DESIGN_CAPACITY 1500

// LoRa
#define USE_SX1262
#if !defined(T5_S3_EPAPER_PRO_H752_02)
#define USE_SX1268
#endif

#define LORA_SCK SCK
#define LORA_MISO MISO
#define LORA_MOSI MOSI
#define LORA_CS 46

#define LORA_DIO0 -1
#if defined(T5_S3_EPAPER_PRO_V1)
#define LORA_RESET 43
#define LORA_DIO1 3  // SX1262 IRQ
#define LORA_DIO2 44 // SX1262 BUSY
#define LORA_DIO3
#else
#define LORA_RESET 1
#define LORA_DIO1 10 // SX1262 IRQ
#define LORA_DIO2 47 // SX1262 BUSY
#define LORA_DIO3
#endif

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_DIO2
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 2.4
#define SX126X_CURRENT_LIMIT 140
#define SX126X_MAX_POWER 22
#define SX126X_USE_REGULATOR_LDO 0
