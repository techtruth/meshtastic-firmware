#pragma once

#include "Power.h"
#include "configuration.h"
#include "concurrency/OSThread.h"
#include "graphics/niche/InkHUD/Applet.h"

#include <vector>

#if defined(MESHTASTIC_INCLUDE_INKHUD) && defined(HAS_BQ27220) && defined(HAS_PPM) && HAS_PPM

namespace NicheGraphics::InkHUD
{

class BatteryApplet : public Applet, public concurrency::OSThread
{
  public:
    BatteryApplet();

    void onRender(bool full) override;
    void onActivate() override;
    void onDeactivate() override;
    void onForeground() override;
    int32_t runOnce() override;
    int onPowerStatusUpdate(const meshtastic::PowerStatus *status);

  private:
    struct Row {
        const char *label;
        std::string value;
    };

    CallbackObserver<BatteryApplet, const meshtastic::PowerStatus *> powerStatusObserver =
        CallbackObserver<BatteryApplet, const meshtastic::PowerStatus *>(this, &BatteryApplet::onPowerStatusUpdate);

    bool observingPower = false;

    int16_t drawSummary(const LipoBatteryTelemetry &telemetry, int16_t top);
    void drawBatteryMeter(int16_t left, int16_t top, uint16_t width, uint16_t height, uint16_t percent);
    int16_t drawSection(const char *title, const std::vector<Row> &rows, int16_t left, int16_t top, uint16_t width);
    std::vector<Row> chargerRows(const LipoBatteryTelemetry &telemetry);
    std::vector<Row> gaugeRows(const LipoBatteryTelemetry &telemetry);
};

} // namespace NicheGraphics::InkHUD

#endif
