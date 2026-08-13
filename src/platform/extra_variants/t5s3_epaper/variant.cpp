#include "configuration.h"

#ifdef T5_S3_EPAPER_PRO

#include "Observer.h"
#include "TouchDrvGT911.hpp"
#include "Wire.h"
#include "buzz.h"
#include "concurrency/OSThread.h"
#include "input/InputBroker.h"
#include "input/TouchScreenImpl1.h"
#include "main.h"
#include "mesh/Throttle.h"
#include "sleep.h"
#include <cstring>

#ifdef ARCH_ESP32
#include <driver/gpio.h>
#include <esp_sleep.h>
#endif

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/Persistence.h"
#include "graphics/niche/InkHUD/SystemApplet.h"

// Bridges touch events from TouchScreenImpl1 directly into InkHUD,
// bypassing the InputBroker (which is excluded in InkHUD builds).
// Routing mirrors the mini-epaper-s3 two-way rocker pattern:
//   - Nav left/right: prevApplet/nextApplet when idle, navUp/Down when a system applet has focus (e.g. menu)
//   - Nav up/down:    navUp/navDown always (menu scroll)
//   - Tap/long-press: direct touch point dispatch (with fallback to short/long button semantics)
class TouchInkHUDBridge : public Observer<const InputEvent *>
{
    int onNotify(const InputEvent *e) override
    {
        auto *inkhud = NicheGraphics::InkHUD::InkHUD::getInstance();

        // Keep alignment in sync with the current rotation so that visual-frame gestures
        // always pass through nav functions without remapping: (rotation + alignment) % 4 == 0.
        inkhud->persistence->settings.joystick.alignment = (4 - inkhud->persistence->settings.rotation) % 4;

        // Check whether a system applet (e.g. menu) is currently handling input
        bool systemHandlingInput = false;
        for (const NicheGraphics::InkHUD::SystemApplet *sa : inkhud->systemApplets) {
            if (sa->handleInput) {
                systemHandlingInput = true;
                break;
            }
        }

        switch (e->inputEvent) {
        case INPUT_BROKER_USER_PRESS:
            inkhud->touchTap(e->touchX, e->touchY);
            break;
        case INPUT_BROKER_SELECT:
            inkhud->touchLongPress(e->touchX, e->touchY);
            break;
        case INPUT_BROKER_LEFT:
            if (systemHandlingInput)
                inkhud->touchNavUp();
            else
                inkhud->prevApplet();
            break;
        case INPUT_BROKER_RIGHT:
            if (systemHandlingInput)
                inkhud->touchNavDown();
            else
                inkhud->nextApplet();
            break;
        case INPUT_BROKER_UP:
            inkhud->touchNavUp();
            break;
        case INPUT_BROKER_DOWN:
            inkhud->touchNavDown();
            break;
        default:
            break;
        }
        return 0;
    }
};

static TouchInkHUDBridge touchBridge;
#endif // MESHTASTIC_INCLUDE_NICHE_GRAPHICS

TouchDrvGT911 touch;

namespace
{
constexpr uint8_t BACKLIGHT_ON_LEVEL = HIGH;
constexpr uint8_t BACKLIGHT_OFF_LEVEL = LOW;
volatile bool backlightUserEnabled = true;
volatile bool backlightForcedByTimeout = false;
volatile bool backlightForcedBySleep = false;

void applyBacklightState()
{
    const bool shouldOn = backlightUserEnabled && !backlightForcedByTimeout && !backlightForcedBySleep;
    digitalWrite(BOARD_BL_EN, shouldOn ? BACKLIGHT_ON_LEVEL : BACKLIGHT_OFF_LEVEL);
}

volatile bool touchInputEnabled = true;
volatile bool touchForcedByTimeout = false;
volatile bool touchControllerReady = false;
volatile bool touchLightSleepActive = false;
volatile bool touchNeedsWake = false;
volatile bool touchIndicatorRefreshPending = false;
// When the light-sleep resume happened, not when the block expires: an interval bounds a missed
// 0-check by the settle time, where a stored deadline would block for up to half a wrap cycle.
constexpr uint32_t TOUCH_RESUME_BLOCK_MS = 150;
volatile uint32_t touchResumeAtMs = 0;
volatile uint32_t touchStateEpoch = 1;
volatile bool homeCapButtonEventsEnabled = false;
#if HAS_SCREEN
uint32_t lastTouchIndicatorMs = 0;
#endif

#if defined(BOARD_PCA9535_ADDR)
constexpr uint8_t PCA9535_REG_INPUT0 = 0x00;
constexpr uint8_t PCA9535_REG_INPUT1 = 0x01;
constexpr uint8_t PCA9535_REG_OUTPUT0 = 0x02;
constexpr uint8_t PCA9535_REG_OUTPUT1 = 0x03;
constexpr uint8_t PCA9535_REG_POLARITY0 = 0x04;
constexpr uint8_t PCA9535_REG_POLARITY1 = 0x05;
constexpr uint8_t PCA9535_REG_CONFIG0 = 0x06;
constexpr uint8_t PCA9535_REG_CONFIG1 = 0x07;

bool writePca9535Register(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(BOARD_PCA9535_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool readPca9535Register(uint8_t reg, uint8_t *value)
{
    if (!value) {
        return false;
    }

    Wire.beginTransmission(BOARD_PCA9535_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((uint8_t)BOARD_PCA9535_ADDR, (uint8_t)1) != 1) {
        return false;
    }

    *value = Wire.read();
    return true;
}

#if !defined(T5_S3_EPAPER_PRO_V1)
enum class Pca9535Direction : uint8_t {
    Output,
    Input,
};

enum class Pca9535BoardState : uint8_t {
    Boot,
    Normal,
    Sleep,
};

constexpr int8_t PCA9535_NOT_DRIVEN = -1;

struct H752V2Pca9535Signal {
    uint8_t pin;
    const char *name;
    Pca9535Direction direction;
    bool inverted;
    int8_t bootValue;
    int8_t normalValue;
    int8_t sleepValue;
    const char *owner;
};

static constexpr H752V2Pca9535Signal h752V2Pca9535Signals[] = {
    {BOARD_PCA9535_LORA_GPS_EN, "LORA_EN", Pca9535Direction::Output, false, 1, 1, 0, "board LoRa/GPS rail"},
    {BOARD_PCA9535_IO0_1_NC, "IO0_1_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_IO0_2_NC, "IO0_2_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_IO0_3_NC, "IO0_3_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_IO0_4_NC, "IO0_4_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_IO0_5_NC, "IO0_5_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_IO0_6_NC, "IO0_6_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_IO0_7_NC, "IO0_7_NC", Pca9535Direction::Output, false, 1, 1, 1, "unused port0 pin held high"},
    {BOARD_PCA9535_EPD_OE, "EPD_OE", Pca9535Direction::Output, false, 0, 0, 0, "display/TPS65185"},
    {BOARD_PCA9535_EPD_MODE, "EPD_MODE", Pca9535Direction::Output, false, 0, 0, 0, "display/TPS65185"},
    {BOARD_PCA9535_BUTTON, "IO48_KEY", Pca9535Direction::Input, false, PCA9535_NOT_DRIVEN, PCA9535_NOT_DRIVEN,
     PCA9535_NOT_DRIVEN, "PCA9535 IO1_2 function key"},
    {BOARD_PCA9535_TPS_PWRUP, "TPS_PWRUP", Pca9535Direction::Output, false, 0, 0, 0, "display/TPS65185"},
    {BOARD_PCA9535_EPD_VCOM_CTRL, "VCOM_CTRL", Pca9535Direction::Output, false, 0, 0, 0, "display/TPS65185"},
    {BOARD_PCA9535_TPS_WAKEUP, "TPS_WAKEUP", Pca9535Direction::Output, false, 0, 0, 0, "display/TPS65185"},
    {BOARD_PCA9535_TPS_PWR_GOOD, "TPS_PWR_GOOD", Pca9535Direction::Input, false, PCA9535_NOT_DRIVEN,
     PCA9535_NOT_DRIVEN, PCA9535_NOT_DRIVEN, "display power-good"},
    {BOARD_PCA9535_TPS_INT, "TPS_INT", Pca9535Direction::Input, false, PCA9535_NOT_DRIVEN, PCA9535_NOT_DRIVEN,
     PCA9535_NOT_DRIVEN, "display/TPS65185 interrupt"},
};

constexpr bool pca9535SignalOnPort(const H752V2Pca9535Signal &signal, uint8_t port)
{
    return (signal.pin >> 3) == port;
}

constexpr uint8_t pca9535Bit(const H752V2Pca9535Signal &signal)
{
    return (uint8_t)(1u << (signal.pin & 0x07));
}

constexpr int8_t pca9535SignalValue(const H752V2Pca9535Signal &signal, Pca9535BoardState state)
{
    switch (state) {
    case Pca9535BoardState::Boot:
        return signal.bootValue;
    case Pca9535BoardState::Normal:
        return signal.normalValue;
    case Pca9535BoardState::Sleep:
        return signal.sleepValue;
    }
    return PCA9535_NOT_DRIVEN;
}

constexpr uint8_t buildPca9535Config(uint8_t port)
{
    uint8_t value = 0;
    for (const auto &signal : h752V2Pca9535Signals) {
        if (pca9535SignalOnPort(signal, port) && signal.direction == Pca9535Direction::Input) {
            value |= pca9535Bit(signal);
        }
    }
    return value;
}

constexpr uint8_t buildPca9535Polarity(uint8_t port)
{
    uint8_t value = 0;
    for (const auto &signal : h752V2Pca9535Signals) {
        if (pca9535SignalOnPort(signal, port) && signal.inverted) {
            value |= pca9535Bit(signal);
        }
    }
    return value;
}

constexpr uint8_t buildPca9535Output(uint8_t port, Pca9535BoardState state)
{
    uint8_t value = 0;
    for (const auto &signal : h752V2Pca9535Signals) {
        const int8_t stateValue = pca9535SignalValue(signal, state);
        if (pca9535SignalOnPort(signal, port) && signal.direction == Pca9535Direction::Output && stateValue > 0) {
            value |= pca9535Bit(signal);
        }
    }
    return value;
}

static_assert(buildPca9535Polarity(0) == BOARD_PCA9535_PORT0_POLARITY, "H752 V2 port0 polarity mismatch");
static_assert(buildPca9535Polarity(1) == BOARD_PCA9535_PORT1_POLARITY, "H752 V2 port1 polarity mismatch");
static_assert(buildPca9535Config(0) == BOARD_PCA9535_PORT0_CONFIG, "H752 V2 port0 config mismatch");
static_assert(buildPca9535Config(1) == BOARD_PCA9535_PORT1_CONFIG, "H752 V2 port1 config mismatch");
static_assert(buildPca9535Output(0, Pca9535BoardState::Boot) == BOARD_PCA9535_PORT0_OUTPUT_BOOT,
              "H752 V2 port0 boot output mismatch");
static_assert(buildPca9535Output(0, Pca9535BoardState::Normal) == BOARD_PCA9535_PORT0_OUTPUT_NORMAL,
              "H752 V2 port0 normal output mismatch");
static_assert(buildPca9535Output(0, Pca9535BoardState::Sleep) == BOARD_PCA9535_PORT0_OUTPUT_SLEEP,
              "H752 V2 port0 sleep output mismatch");
static_assert(buildPca9535Output(1, Pca9535BoardState::Boot) == BOARD_PCA9535_PORT1_OUTPUT_BOOT,
              "H752 V2 port1 boot output mismatch");
static_assert(buildPca9535Output(1, Pca9535BoardState::Normal) == BOARD_PCA9535_PORT1_OUTPUT_EPD_OFF,
              "H752 V2 port1 normal output mismatch");
static_assert(buildPca9535Output(1, Pca9535BoardState::Sleep) == BOARD_PCA9535_PORT1_OUTPUT_SLEEP,
              "H752 V2 port1 sleep output mismatch");

const char *pca9535DirectionName(Pca9535Direction direction)
{
    return direction == Pca9535Direction::Input ? "input" : "output";
}

void logH752V2Pca9535Map()
{
    static bool logged = false;
    if (logged) {
        return;
    }
    logged = true;

    for (const auto &signal : h752V2Pca9535Signals) {
        LOG_DEBUG("H752 V2 PCA9535 pin %u %-13s %s boot=%d normal=%d sleep=%d owner=%s", signal.pin, signal.name,
                  pca9535DirectionName(signal.direction), signal.bootValue, signal.normalValue, signal.sleepValue,
                  signal.owner);
    }
}

void forceBacklightOffForDeepSleep()
{
    backlightForcedBySleep = true;
    pinMode(BOARD_BL_EN, OUTPUT);
    applyBacklightState();
}

#ifdef ARCH_ESP32
void holdOutputLowForDeepSleep(uint8_t pin)
{
    gpio_num_t gpio = (gpio_num_t)pin;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        return;
    }

    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    esp_err_t err = gpio_hold_en(gpio);
    if (err != ESP_OK) {
        LOG_WARN("H752 V2 gpio_hold_en(%u) failed: %d", pin, err);
    }
}

void prepareH752V2DirectPinsForDeepSleep()
{
    holdOutputLowForDeepSleep(GT911_PIN_RST);
    holdOutputLowForDeepSleep(LORA_RESET);
    forceBacklightOffForDeepSleep();
    gpio_deep_sleep_hold_en();
}
#else
void prepareH752V2DirectPinsForDeepSleep()
{
    forceBacklightOffForDeepSleep();
}
#endif

bool configurePca9535ForH752V2()
{
    bool ok = true;

    logH752V2Pca9535Map();
    ok = writePca9535Register(PCA9535_REG_POLARITY0, buildPca9535Polarity(0)) && ok;
    ok = writePca9535Register(PCA9535_REG_POLARITY1, buildPca9535Polarity(1)) && ok;
    ok = writePca9535Register(PCA9535_REG_OUTPUT0, buildPca9535Output(0, Pca9535BoardState::Boot)) && ok;
    ok = writePca9535Register(PCA9535_REG_OUTPUT1, buildPca9535Output(1, Pca9535BoardState::Boot)) && ok;
    ok = writePca9535Register(PCA9535_REG_CONFIG0, buildPca9535Config(0)) && ok;
    ok = writePca9535Register(PCA9535_REG_CONFIG1, buildPca9535Config(1)) && ok;

    uint8_t ignored = 0xFF;
    (void)readPca9535Register(PCA9535_REG_INPUT0, &ignored);
    (void)readPca9535Register(PCA9535_REG_INPUT1, &ignored); // clear any latched IO48-key/TPS interrupt

    if (ok) {
        LOG_INFO("H752 V2 PCA9535 initialized: port0=0x%02x config0=0x%02x port1=0x%02x config1=0x%02x",
                 buildPca9535Output(0, Pca9535BoardState::Boot), buildPca9535Config(0),
                 buildPca9535Output(1, Pca9535BoardState::Boot), buildPca9535Config(1));
    } else {
        LOG_WARN("H752 V2 PCA9535 init failed");
    }

    return ok;
}

bool setSharedLoraGpsRail(bool enabled)
{
    const uint8_t output0 = buildPca9535Output(0, enabled ? Pca9535BoardState::Normal : Pca9535BoardState::Sleep);
    bool ok = writePca9535Register(PCA9535_REG_OUTPUT0, output0);
    ok = writePca9535Register(PCA9535_REG_CONFIG0, buildPca9535Config(0)) && ok;
    if (!ok) {
        LOG_WARN("H752 V2 shared LoRa/GPS rail %s failed", enabled ? "enable" : "disable");
    }
    return ok;
}

struct BoardPowerSleepObserver {
    void begin()
    {
        if (registered) {
            return;
        }
        deepSleepObserver.observe(&notifyDeepSleep);
#ifdef ARCH_ESP32
        lightSleepObserver.observe(&notifyLightSleep);
        lightSleepEndObserver.observe(&notifyLightSleepEnd);
#endif
        registered = true;
    }

    int onDeepSleep(void *)
    {
        prepareH752V2DirectPinsForDeepSleep();
        setSharedLoraGpsRail(false);
        return 0;
    }

#ifdef ARCH_ESP32
    int onLightSleep(void *)
    {
        setSharedLoraGpsRail(true);
        return 0;
    }

    int onLightSleepEnd(esp_sleep_wakeup_cause_t)
    {
        setSharedLoraGpsRail(true);
        return 0;
    }
#endif

    bool registered = false;
    CallbackObserver<BoardPowerSleepObserver, void *> deepSleepObserver{this, &BoardPowerSleepObserver::onDeepSleep};
#ifdef ARCH_ESP32
    CallbackObserver<BoardPowerSleepObserver, void *> lightSleepObserver{this, &BoardPowerSleepObserver::onLightSleep};
    CallbackObserver<BoardPowerSleepObserver, esp_sleep_wakeup_cause_t> lightSleepEndObserver{this,
                                                                                              &BoardPowerSleepObserver::onLightSleepEnd};
#endif
} static boardPowerSleepObserver;
#endif // !T5_S3_EPAPER_PRO_V1
#endif // BOARD_PCA9535_ADDR

void showTouchIndicator(const char *text)
{
#if HAS_SCREEN
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
    // InkHUD builds render a dedicated bottom-edge "TOUCH OFF" overlay instead of popup banners.
    (void)text;
    return;
#else
    // Keep repeated notifications low profile and non-spammy.
    if ((millis() - lastTouchIndicatorMs) < 500) {
        return;
    }
    lastTouchIndicatorMs = millis();
    if (screen) {
        screen->showSimpleBanner(text, 1400);
    }
#endif
#else
    (void)text;
#endif
}

#if defined(BOARD_PCA9535_ADDR) && defined(BOARD_PCA9535_BUTTON_MASK)
bool readPca9535Port1(uint8_t *value)
{
    return readPca9535Register(PCA9535_REG_INPUT1, value);
}

bool isPca9535Io48KeyPressed()
{
    uint8_t port1 = 0xFF;
    if (!readPca9535Port1(&port1)) {
        return false;
    }

    return (port1 & BOARD_PCA9535_BUTTON_MASK) == 0;
}

class Pca9535KeyInterruptThread : public concurrency::OSThread
{
  public:
    Pca9535KeyInterruptThread() : concurrency::OSThread("t5s3PCA9535Int", SAMPLE_MS)
    {
        // Do not run unless an edge arrives.
        OSThread::disable();
        instance = this;
#ifdef ARCH_ESP32
        lsObserver.observe(&notifyLightSleep);
        lsEndObserver.observe(&notifyLightSleepEnd);
#endif
    }

    void begin()
    {
        pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
        attachInterrupt(BOARD_PCA9535_INT, Pca9535KeyInterruptThread::isr, FALLING);
    }

  protected:
    int32_t runOnce() override
    {
        const uint32_t now = millis();

        // 0 means the device has never light-slept, so no block is armed - test it first.
        if (touchResumeAtMs != 0 && Throttle::isWithinTimespanMs(touchResumeAtMs, TOUCH_RESUME_BLOCK_MS)) {
            resetStateAndStop();
            return OSThread::disable();
        }

        if (touchLightSleepActive) {
            resetStateAndStop();
            return OSThread::disable();
        }

        // Ignore IO48-key handling while BOOT/user button is held.
        if (digitalRead(BUTTON_PIN) == LOW) {
            resetStateAndStop();
            return OSThread::disable();
        }

        switch (state) {
        case State::IRQ_PENDING: {
            // Initial debounce after expander interrupt edge.
            if ((uint32_t)(now - irqAtMs) < DEBOUNCE_MS) {
                return SAMPLE_MS;
            }

            if (isPca9535Io48KeyPressed()) {
                state = State::PRESSED;
                pressStartMs = now;
                return SAMPLE_MS;
            }

            // Spurious/cleared edge.
            resetStateAndStop();
            return OSThread::disable();
        }

        case State::PRESSED: {
            if (isPca9535Io48KeyPressed()) {
                // Fire long-press action as soon as threshold is reached, without waiting for release.
                if (!longPressFired && (uint32_t)(now - pressStartMs) >= LONG_PRESS_MIN_MS &&
                    (uint32_t)(now - lastActionMs) >= ACTION_COOLDOWN_MS) {
                    LOG_INFO("H752 V2 IO48 key long press: toggle backlight");
                    t5BacklightToggleUser();
                    longPressFired = true;
                    lastActionMs = now;
                }
                return SAMPLE_MS;
            }

            // Released: if long-press already fired, do nothing. Otherwise classify short press.
            const uint32_t heldMs = now - pressStartMs;
            if (!longPressFired && heldMs >= SHORT_PRESS_MIN_MS && (uint32_t)(now - lastActionMs) >= ACTION_COOLDOWN_MS) {
                // If timeout forced touch/backlight off, short-press acts as a wake action first.
                if (t5TouchIsForcedByTimeout()) {
                    LOG_INFO("H752 V2 IO48 key short press: resume touch/backlight");
                    t5TouchHandleUserInput();
                    t5BacklightHandleUserInput();
                } else {
                    LOG_INFO("H752 V2 IO48 key short press: toggle touch input");
                    toggleTouchInputEnabled();
                }
                lastActionMs = now;
            }

            resetStateAndStop();
            return OSThread::disable();
        }

        case State::REST:
        default:
            return OSThread::disable();
        }
    }

  private:
    enum class State : uint8_t {
        REST,
        IRQ_PENDING,
        PRESSED,
    };

    static constexpr uint32_t SAMPLE_MS = 15;
    static constexpr uint32_t DEBOUNCE_MS = 25;
    static constexpr uint32_t SHORT_PRESS_MIN_MS = 30;
    static constexpr uint32_t LONG_PRESS_MIN_MS = 450;
    static constexpr uint32_t ACTION_COOLDOWN_MS = 180;

    static Pca9535KeyInterruptThread *instance;

    static void isr()
    {
        if (instance) {
            instance->onInterruptEdge();
        }
    }

    void onInterruptEdge()
    {
        if (touchLightSleepActive) {
            return;
        }
        // See the runOnce() guard above for why 0 must be tested separately.
        if (touchResumeAtMs != 0 && Throttle::isWithinTimespanMs(touchResumeAtMs, TOUCH_RESUME_BLOCK_MS)) {
            return;
        }
        if (state != State::REST) {
            return;
        }

        state = State::IRQ_PENDING;
        irqAtMs = millis();
        startThread();
    }

    void startThread()
    {
        if (!OSThread::enabled) {
            OSThread::setIntervalFromNow(0);
            OSThread::enabled = true;
            runASAP = true;
        }
    }

    void resetStateAndStop()
    {
        state = State::REST;
        longPressFired = false;
        if (OSThread::enabled) {
            OSThread::disable();
        }
    }

#ifdef ARCH_ESP32
    int onLightSleep(void *)
    {
        detachInterrupt(BOARD_PCA9535_INT);
        uint8_t ignored = 0xFF;
        (void)readPca9535Port1(&ignored);
        resetStateAndStop();
        return 0;
    }

    int onLightSleepEnd(esp_sleep_wakeup_cause_t cause)
    {
        (void)cause;
        // Consume any pending interrupt source before reattaching the awake-mode ISR.
        uint8_t ignored = 0xFF;
        (void)readPca9535Port1(&ignored);
        pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
        attachInterrupt(BOARD_PCA9535_INT, Pca9535KeyInterruptThread::isr, FALLING);

        return 0;
    }

    CallbackObserver<Pca9535KeyInterruptThread, void *> lsObserver{this, &Pca9535KeyInterruptThread::onLightSleep};
    CallbackObserver<Pca9535KeyInterruptThread, esp_sleep_wakeup_cause_t> lsEndObserver{
        this, &Pca9535KeyInterruptThread::onLightSleepEnd};
#endif

    volatile State state = State::REST;
    volatile uint32_t irqAtMs = 0;
    uint32_t pressStartMs = 0;
    bool longPressFired = false;
    uint32_t lastActionMs = 0;
};

Pca9535KeyInterruptThread *Pca9535KeyInterruptThread::instance = nullptr;
Pca9535KeyInterruptThread *pca9535KeyThread = nullptr;
#endif

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
void refreshTouchIndicatorInInkHUD(bool async = true)
{
    auto *inkhud = NicheGraphics::InkHUD::InkHUD::getInstance();
    NicheGraphics::InkHUD::SystemApplet *touchStatus = nullptr;
    for (auto *sa : inkhud->systemApplets) {
        if (sa && sa->name && strcmp(sa->name, "TouchStatus") == 0) {
            touchStatus = sa;
            break;
        }
    }

    if (touchStatus) {
        if (inkhud->isTouchEnabled())
            touchStatus->sendToBackground();
        else
            touchStatus->bringToForeground();
    }

    // Re-render all applets so touch-status visibility changes are immediately reflected.
    inkhud->forceUpdate(NicheGraphics::Drivers::EInk::UpdateTypes::FAST, true, async);
}
#endif

} // namespace

void t5BacklightSetUserEnabled(bool enabled)
{
    backlightUserEnabled = enabled;
    if (enabled) {
        // Manual ON should release auto-off gates.
        backlightForcedByTimeout = false;
        backlightForcedBySleep = false;
    }
    applyBacklightState();
}

bool t5BacklightIsUserEnabled()
{
    return backlightUserEnabled;
}

void t5BacklightToggleUser()
{
    t5BacklightSetUserEnabled(!backlightUserEnabled);
}

void t5BacklightSetForcedByTimeout(bool forced)
{
    backlightForcedByTimeout = forced;
    applyBacklightState();
}

void t5BacklightSetForcedBySleep(bool forced)
{
    backlightForcedBySleep = forced;
    applyBacklightState();
}

void t5BacklightHandleUserInput()
{
    // Screen-timeout should be lifted by direct user interaction.
    backlightForcedByTimeout = false;
    applyBacklightState();
}

void t5TouchSetForcedByTimeout(bool forced)
{
    if (touchForcedByTimeout == forced) {
        return;
    }

    touchForcedByTimeout = forced;
    touchStateEpoch++;
    touchIndicatorRefreshPending = true;

    if (forced) {
        // Timeout only gates touch input in software. Avoid GT911 I2C here because
        // PowerFSM same-state transitions can fire from phone contact and screen timeout.
        touchNeedsWake = false;
    } else if (touchInputEnabled && touchControllerReady && !touchLightSleepActive) {
        touchNeedsWake = false;
    }

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
    if (!touchLightSleepActive) {
        refreshTouchIndicatorInInkHUD();
        touchIndicatorRefreshPending = false;
    }
#endif
}

bool t5TouchIsForcedByTimeout()
{
    return touchForcedByTimeout;
}

void t5TouchHandleUserInput()
{
    t5TouchSetForcedByTimeout(false);
}

void t5SetHomeCapButtonEventsEnabled(bool enabled)
{
    homeCapButtonEventsEnabled = enabled;
}

bool isTouchInputEnabled()
{
    return touchInputEnabled && !touchForcedByTimeout && !touchLightSleepActive;
}

void setTouchInputEnabled(bool enabled, bool showIndicator)
{
    if (touchInputEnabled == enabled) {
        LOG_DEBUG("touchscreen1: setTouchInputEnabled no-op en=%d", enabled);
        return;
    }

    LOG_DEBUG("touchscreen1: setTouchInputEnabled %d -> %d (showIndicator=%d)", touchInputEnabled, enabled, showIndicator);
    touchInputEnabled = enabled;
    touchStateEpoch++;

    if (enabled) {
        touchNeedsWake = touchControllerReady;
        if (touchControllerReady && !touchLightSleepActive) {
            LOG_DEBUG("touchscreen1: wakeup() on enable");
            touch.wakeup();
            touchNeedsWake = false;
        }
    } else {
        touchNeedsWake = false;
        if (touchControllerReady && !touchLightSleepActive) {
            LOG_DEBUG("touchscreen1: sleep() on disable");
            touch.sleep();
        }
        if (showIndicator) {
            showTouchIndicator("Touch OFF");
            touchIndicatorRefreshPending = true;
        }
    }

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
    if (showIndicator && !touchLightSleepActive) {
        refreshTouchIndicatorInInkHUD();
        touchIndicatorRefreshPending = false;
    }
#endif
}

void toggleTouchInputEnabled()
{
    setTouchInputEnabled(!touchInputEnabled, true);
}

void postI2CInitVariant()
{
#if defined(BOARD_PCA9535_ADDR) && !defined(T5_S3_EPAPER_PRO_V1)
    configurePca9535ForH752V2();
#endif
}

// Commands the GT911 into standby before the Wire bus is torn down.
// notifyDeepSleep fires before Wire.end() in doDeepSleep(), so I2C is still available here.
struct TouchDeepSleepObserver {
    int onDeepSleep(void *)
    {
        touch.sleep();
        return 0;
    }
    CallbackObserver<TouchDeepSleepObserver, void *> observer{this, &TouchDeepSleepObserver::onDeepSleep};
} static touchDeepSleepObserver;

#ifdef ARCH_ESP32
struct TouchLightSleepObserver {
    int onLightSleep(void *)
    {
        touchLightSleepActive = true;
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
        // Render touch-off overlay before sleeping so user sees touch is unavailable.
        touchIndicatorRefreshPending = true;
        refreshTouchIndicatorInInkHUD(false);
        touchIndicatorRefreshPending = false;
#endif
        return 0;
    }

    CallbackObserver<TouchLightSleepObserver, void *> observer{this, &TouchLightSleepObserver::onLightSleep};
} static touchLightSleepObserver;

struct TouchLightSleepEndObserver {
    int onLightSleepEnd(esp_sleep_wakeup_cause_t cause)
    {
        (void)cause;
        touchLightSleepActive = false;

        if (!touchControllerReady) {
            return 0;
        }

        if (touchInputEnabled && !touchForcedByTimeout) {
            touchNeedsWake = true;
        } else {
            touchNeedsWake = false;
        }

        touchStateEpoch++;
        touchResumeAtMs = millis();
        touchIndicatorRefreshPending = !isTouchInputEnabled();
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
        // Clear sleep-time touch overlay after wake.
        touchIndicatorRefreshPending = true;
        refreshTouchIndicatorInInkHUD();
        touchIndicatorRefreshPending = false;
#endif
        return 0;
    }

    CallbackObserver<TouchLightSleepEndObserver, esp_sleep_wakeup_cause_t> observer{this,
                                                                                    &TouchLightSleepEndObserver::onLightSleepEnd};
} static touchLightSleepEndObserver;
#endif

bool readTouch(int16_t *x, int16_t *y)
{
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
    constexpr uint32_t TOUCH_WAKE_SUPPRESS_MS = 60;
    static uint32_t suppressFromMs = 0; // 0 = not suppressing, same reading as touchResumeAtMs
    static uint32_t seenTouchStateEpoch = 0;

    // Reset transient gesture helpers whenever touch mode changes.
    if (seenTouchStateEpoch != touchStateEpoch) {
        seenTouchStateEpoch = touchStateEpoch;
        suppressFromMs = 0;
    }

    // Let buses and peripherals settle briefly after light-sleep wake. 0 means no wake yet.
    if (touchResumeAtMs != 0 && Throttle::isWithinTimespanMs(touchResumeAtMs, TOUCH_RESUME_BLOCK_MS)) {
        return false;
    }

    if (touchIndicatorRefreshPending) {
        refreshTouchIndicatorInInkHUD();
        touchIndicatorRefreshPending = false;
    }

    if (!isTouchInputEnabled()) {
        return false;
    }

    if (touchNeedsWake && touchControllerReady) {
        LOG_DEBUG("touchscreen1: wakeup() on deferred resume");
        touch.wakeup();
        touchNeedsWake = false;
        suppressFromMs = millis();
        return false;
    }

    // After a recovery pulse, emit a brief "released" window so gesture state can reset.
    if (suppressFromMs != 0 && Throttle::isWithinTimespanMs(suppressFromMs, TOUCH_WAKE_SUPPRESS_MS)) {
        return false;
    }
#endif

    if (!digitalRead(GT911_PIN_INT)) {
        int16_t raw_x;
        int16_t raw_y;
        if (touch.getPoint(&raw_x, &raw_y)) {
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
            // Transform raw GT911 axes to visual-frame coordinates for the current display rotation.
            // rotation=3 is the physical identity (device's default orientation).
            switch (NicheGraphics::InkHUD::InkHUD::getInstance()->persistence->settings.rotation) {
            default:
            case 3:
                *x = raw_x;
                *y = raw_y;
                break; // identity
            case 2:
                *x = (EPD_WIDTH - 1) - raw_y;
                *y = raw_x;
                break; // 90° CW tilt
            case 1:
                *x = (EPD_HEIGHT - 1) - raw_x;
                *y = (EPD_WIDTH - 1) - raw_y;
                break; // 180° flip
            case 0:
                *x = raw_y;
                *y = (EPD_HEIGHT - 1) - raw_x;
                break; // 90° CCW tilt
            }
#else
            *x = raw_x;
            *y = raw_y;
#endif
            LOG_DEBUG("touched(%d/%d)", *x, *y);
            return true;
        }
    }

    return false;
}

void variant_shutdown()
{
    // Ensure backlight is off during deep sleep.
    t5BacklightSetForcedBySleep(true);
}

void lateInitVariant()
{
    touch.setPins(GT911_PIN_RST, GT911_PIN_INT);
    if (touch.begin(Wire, GT911_SLAVE_ADDRESS_H, GT911_PIN_SDA, GT911_PIN_SCL)) {
        // Wire the GT911 center/home capacitive key into Meshtastic input handling.
        touch.setHomeButtonCallback(
            [](void *user_data) {
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
                if (!homeCapButtonEventsEnabled) {
                    return;
                }

                static uint32_t lastHomeMs = 0;
                const uint32_t now = millis();
                if ((uint32_t)(now - lastHomeMs) < 220) {
                    return; // debounce repeated key reports while still touched
                }
                lastHomeMs = now;

                auto *inkhud = NicheGraphics::InkHUD::InkHUD::getInstance();
                if (inkhud) {
                    // Route through InkHUD EXIT/HOME path (menu close, etc).
                    inkhud->exitShort();
                }
#else
                (void)user_data;
#endif
            },
            nullptr);
        touchControllerReady = true;
        touchInputEnabled = true;
        touchForcedByTimeout = false;
        touchLightSleepActive = false;
        touchStateEpoch++;
        touchDeepSleepObserver.observer.observe(&notifyDeepSleep);
#ifdef ARCH_ESP32
        touchLightSleepObserver.observer.observe(&notifyLightSleep);
        touchLightSleepEndObserver.observer.observe(&notifyLightSleepEnd);
#endif
        touchScreenImpl1 = new TouchScreenImpl1(EPD_WIDTH, EPD_HEIGHT, readTouch);
        touchScreenImpl1->init();
#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
        touchBridge.observe(touchScreenImpl1);
#endif
    } else {
        touchControllerReady = false;
        LOG_ERROR("Failed to find touch controller");
    }

#if defined(BOARD_PCA9535_ADDR) && defined(BOARD_PCA9535_BUTTON_MASK)
    // Start IO48-key interrupt handling after touch init is complete.
    if (!pca9535KeyThread) {
        pca9535KeyThread = new Pca9535KeyInterruptThread();
        pca9535KeyThread->begin();
    }
#endif

#if defined(BOARD_PCA9535_ADDR) && !defined(T5_S3_EPAPER_PRO_V1)
    boardPowerSleepObserver.begin();
#endif
}
#endif
