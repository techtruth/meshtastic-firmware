#include "./BatteryApplet.h"

#if defined(MESHTASTIC_INCLUDE_INKHUD) && defined(HAS_BQ27220) && defined(HAS_PPM) && HAS_PPM

#include <algorithm>
#include <cstdio>

using namespace NicheGraphics;

namespace
{
constexpr uint16_t INVALID_GAUGE_TIME = 0xFFFF;
constexpr uint16_t STATUS_DSG = 1u << 0;
constexpr uint16_t STATUS_SYSDWN = 1u << 1;
constexpr uint16_t STATUS_TDA = 1u << 2;
constexpr uint16_t STATUS_BATTPRES = 1u << 3;
constexpr uint16_t STATUS_AUTH_GD = 1u << 4;
constexpr uint16_t STATUS_OCVGD = 1u << 5;
constexpr uint16_t STATUS_TCA = 1u << 6;
constexpr uint16_t STATUS_CHGING = 1u << 8;
constexpr uint16_t STATUS_FC = 1u << 9;
constexpr uint16_t STATUS_OTD = 1u << 10;
constexpr uint16_t STATUS_OTC = 1u << 11;
constexpr uint16_t STATUS_SLEEP = 1u << 12;
constexpr uint16_t STATUS_OCVFALL = 1u << 13;
constexpr uint16_t STATUS_OCVCOMP = 1u << 14;
constexpr uint16_t STATUS_FD = 1u << 15;

std::string safeText(const char *text)
{
    return text ? text : "--";
}

std::string formatMv(uint16_t mv)
{
    if (mv == 0)
        return "--";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f V", mv / 1000.0f);
    return buf;
}

std::string formatMa(uint16_t ma)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u mA", ma);
    return buf;
}

std::string formatSignedMa(int16_t ma)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d mA", ma);
    return buf;
}

std::string formatMah(uint16_t mah)
{
    if (mah == 0)
        return "--";
    char buf[16];
    snprintf(buf, sizeof(buf), "%u mAh", mah);
    return buf;
}

std::string formatPct(uint16_t pct)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%u%%", pct);
    return buf;
}

std::string formatTemp(uint16_t deciKelvin)
{
    if (deciKelvin == 0)
        return "--";

    char buf[16];
    if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
        const float fahrenheit = (((deciKelvin / 10.0f) - 273.15f) * 9.0f / 5.0f) + 32.0f;
        snprintf(buf, sizeof(buf), "%.1f F", fahrenheit);
    } else {
        const float celsius = (deciKelvin / 10.0f) - 273.15f;
        snprintf(buf, sizeof(buf), "%.1f C", celsius);
    }
    return buf;
}

std::string formatMinutes(uint16_t minutes)
{
    if (minutes == INVALID_GAUGE_TIME)
        return "--";
    if (minutes < 60) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u min", minutes);
        return buf;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%uh %02u", minutes / 60, minutes % 60);
    return buf;
}

void appendStatusPart(std::string &status, const char *part)
{
    if (!status.empty())
        status += ", ";
    status += part;
}

std::string formatGaugeStatus(uint16_t status)
{
    if (status == 0)
        return "--";

    std::string text;

    appendStatusPart(text, (status & STATUS_BATTPRES) ? "Present" : "No battery");

    if (status & STATUS_SYSDWN)
        appendStatusPart(text, "System down");
    else if (status & STATUS_FC)
        appendStatusPart(text, "Full");
    else if (status & STATUS_FD)
        appendStatusPart(text, "Empty");
    else if (status & STATUS_DSG)
        appendStatusPart(text, "Discharging");
    else if (status & STATUS_CHGING)
        appendStatusPart(text, "Charge inhibited");
    else
        appendStatusPart(text, "Relaxing");

    if (status & (STATUS_OTC | STATUS_OTD))
        appendStatusPart(text, "Overtemp");
    else if (status & STATUS_TCA)
        appendStatusPart(text, "Charge alarm");
    else if (status & STATUS_TDA)
        appendStatusPart(text, "Discharge alarm");
    else if (status & STATUS_SLEEP)
        appendStatusPart(text, "Gauge sleep");
    else if (status & STATUS_OCVFALL)
        appendStatusPart(text, "OCV failed");
    else if (status & (STATUS_OCVGD | STATUS_OCVCOMP))
        appendStatusPart(text, "OCV OK");
    else if (status & STATUS_AUTH_GD)
        appendStatusPart(text, "Auth OK");

    return text;
}
} // namespace

InkHUD::BatteryApplet::BatteryApplet() : concurrency::OSThread("BatteryApplet")
{
    OSThread::disable();
}

void InkHUD::BatteryApplet::onActivate()
{
    if (power && !observingPower) {
        powerStatusObserver.observe(&power->newStatus);
        observingPower = true;
    }

    OSThread::enabled = true;
    OSThread::setIntervalFromNow(60 * 1000UL);
}

void InkHUD::BatteryApplet::onDeactivate()
{
    if (power && observingPower) {
        powerStatusObserver.unobserve(&power->newStatus);
        observingPower = false;
    }

    OSThread::disable();
}

void InkHUD::BatteryApplet::onForeground()
{
    requestUpdate();
}

int32_t InkHUD::BatteryApplet::runOnce()
{
    if (isForeground())
        requestUpdate(Drivers::EInk::UpdateTypes::FAST);

    return 60 * 1000UL;
}

int InkHUD::BatteryApplet::onPowerStatusUpdate(const meshtastic::PowerStatus *status)
{
    (void)status;
    if (isActive() && isForeground())
        requestUpdate(Drivers::EInk::UpdateTypes::FAST);

    return 0;
}

void InkHUD::BatteryApplet::onRender(bool full)
{
    (void)full;

    drawHeader("Battery");

    LipoBatteryTelemetry telemetry;
    if (!getLipoBatteryTelemetry(telemetry)) {
        printAt(X(0.5), Y(0.5), "Battery hardware not ready", CENTER, MIDDLE);
        return;
    }

    const int16_t contentTop = drawSummary(telemetry, getHeaderHeight() + 6);
    const uint16_t gap = 12;

    if (width() >= 520) {
        const uint16_t colW = (width() - gap) / 2;
        drawSection("BQ25896 Charger", chargerRows(telemetry), 0, contentTop, colW);
        drawSection("BQ27220 Gauge", gaugeRows(telemetry), colW + gap, contentTop, colW);
    } else {
        int16_t y = drawSection("BQ25896 Charger", chargerRows(telemetry), 0, contentTop, width());
        y += gap;
        drawSection("BQ27220 Gauge", gaugeRows(telemetry), 0, y, width());
    }
}

int16_t InkHUD::BatteryApplet::drawSummary(const LipoBatteryTelemetry &telemetry, int16_t top)
{
    const uint16_t percent = std::min<uint16_t>(telemetry.gaugeSocPct, 100);
    const uint16_t meterH = std::max<uint16_t>(24, fontMedium.lineHeight());
    const bool compact = width() < 360;

    if (compact) {
        drawBatteryMeter(0, top, width(), meterH, percent);
        top += meterH + 4;

        setFont(fontMedium);
        printAt(0, top, formatPct(telemetry.gaugeSocPct), LEFT, TOP);
        printAt(width() - 1, top, formatMv(telemetry.gaugeVoltageMv), RIGHT, TOP);
        top += fontMedium.lineHeight();
    } else {
        const uint16_t meterW = std::min<uint16_t>(220, width() / 3);
        drawBatteryMeter(0, top, meterW, meterH, percent);

        std::string headline = formatPct(telemetry.gaugeSocPct) + "  " + formatMv(telemetry.gaugeVoltageMv);
        setFont(fontLarge);
        if (getTextWidth(headline) > width() - meterW - 8)
            setFont(fontMedium);
        printAt(width() - 1, top + (meterH / 2), headline, RIGHT, MIDDLE);
        top += meterH + 4;
    }

    std::string state;
    if (telemetry.chargerVbusIn)
        state = telemetry.chargerChargeDone ? "Charge done" : (telemetry.chargerCharging ? "Charging" : "USB present");
    else
        state = "Discharging";

    const std::string time = telemetry.chargerCharging ? ("Time till full " + formatMinutes(telemetry.gaugeTimeToFullMin))
                                                       : ("Time till empty " + formatMinutes(telemetry.gaugeTimeToEmptyMin));

    setFont(fontSmall);
    printAt(0, top, state, LEFT, TOP);
    printAt(width() - 1, top, time, RIGHT, TOP);
    top += fontSmall.lineHeight();

    printAt(0, top, "Health " + formatPct(telemetry.gaugeSohPct), LEFT, TOP);
    printAt(width() - 1, top, formatSignedMa(telemetry.gaugeCurrentMa), RIGHT, TOP);
    top += fontSmall.lineHeight() + 8;

    return top;
}

void InkHUD::BatteryApplet::drawBatteryMeter(int16_t left, int16_t top, uint16_t width, uint16_t height, uint16_t percent)
{
    if (width < 10 || height < 8)
        return;

    percent = std::min<uint16_t>(percent, 100);

    const uint16_t bumpW = 4;
    const uint16_t bodyW = width - bumpW - 1;
    const uint16_t bumpH = std::max<uint16_t>(4, height / 3);
    const int16_t bumpT = top + ((height - bumpH) / 2);

    drawRect(left, top, bodyW, height, BLACK);
    fillRect(left + bodyW, bumpT, bumpW, bumpH, BLACK);

    const uint16_t innerW = bodyW > 4 ? bodyW - 4 : 0;
    const uint16_t innerH = height > 4 ? height - 4 : 0;
    const uint16_t fillW = (innerW * percent) / 100;
    if (fillW > 0 && innerH > 0)
        fillRect(left + 2, top + 2, fillW, innerH, BLACK);
}

int16_t InkHUD::BatteryApplet::drawSection(const char *title, const std::vector<Row> &rows, int16_t left, int16_t top,
                                           uint16_t width)
{
    if (width == 0 || top >= height())
        return top;

    setFont(fontMedium);
    if (getTextWidth(title) > width)
        setFont(fontSmall);
    printAt(left, top, title, LEFT, TOP);
    top += getFont().lineHeight();

    drawLine(left, top, left + width - 1, top, BLACK);
    top += 4;

    setFont(fontSmall);
    const uint16_t lineH = fontSmall.lineHeight();
    const uint16_t maxLabelW = std::max<uint16_t>(1, (width * 45) / 100);

    for (const Row &row : rows) {
        if (top + lineH > height())
            break;

        const uint16_t measuredLabelW = std::min<uint16_t>(width, getTextWidth(row.label) + 8);
        const uint16_t labelW = std::min<uint16_t>(maxLabelW, std::max<uint16_t>(1, measuredLabelW));
        const uint16_t valueW = width > labelW ? width - labelW : 1;

        setCrop(left, top, labelW, lineH);
        printAt(left, top, row.label, LEFT, TOP);

        setCrop(left + labelW, top, valueW, lineH);
        printAt(left + width - 1, top, row.value, RIGHT, TOP);

        resetCrop();
        top += lineH;
    }

    return top;
}

std::vector<InkHUD::BatteryApplet::Row> InkHUD::BatteryApplet::chargerRows(const LipoBatteryTelemetry &telemetry)
{
    return {
        {"Input", telemetry.chargerVbusIn ? "Connected" : "Disconnected"},
        {"Charge", telemetry.chargerStatus ? safeText(telemetry.chargerStatus) : "--"},
        {"VBUS", formatMv(telemetry.chargerVbusMv)},
        {"VSYS", formatMv(telemetry.chargerVsysMv)},
        {"VBAT", formatMv(telemetry.chargerVbatMv)},
        {"Target", formatMv(telemetry.chargerTargetMv)},
        {"IIN Limit", formatMa(telemetry.chargerInputLimitMa)},
        {"Fast Limit", formatMa(telemetry.chargerFastLimitMa)},
        {"Charge Current", formatMa(telemetry.chargerCurrentMa)},
        {"Precharge", formatMa(telemetry.chargerPrechargeMa)},
        {"Bus", safeText(telemetry.chargerBusStatus)},
        {"NTC", safeText(telemetry.chargerNtcStatus)},
    };
}

std::vector<InkHUD::BatteryApplet::Row> InkHUD::BatteryApplet::gaugeRows(const LipoBatteryTelemetry &telemetry)
{
    std::string finish;
    if (telemetry.chargerVbusIn)
        finish = telemetry.gaugeChargeDone ? "Finish" : "Charging";
    else
        finish = "Discharge";

    return {
        {"Mode", telemetry.gaugeCharging ? "Charging" : "Discharging"},
        {"Finish", finish},
        {"Status", formatGaugeStatus(telemetry.gaugeStatus)},
        {"Voltage", formatMv(telemetry.gaugeVoltageMv)},
        {"Current", formatSignedMa(telemetry.gaugeCurrentMa)},
        {"Temp", formatTemp(telemetry.gaugeTemperatureDk)},
        {"Remaining", formatMah(telemetry.gaugeRemainingMah)},
        {"Full Capacity", formatMah(telemetry.gaugeFullMah)},
        {"Designed for", formatMah(telemetry.gaugeDesignMah)},
        {"State of Charge", formatPct(telemetry.gaugeSocPct)},
        {"State of Health", formatPct(telemetry.gaugeSohPct)},
        {"Time till full", formatMinutes(telemetry.gaugeTimeToFullMin)},
        {"Time till empty", formatMinutes(telemetry.gaugeTimeToEmptyMin)},
    };
}

#endif
