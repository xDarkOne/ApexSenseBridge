#ifdef NDEBUG
#undef NDEBUG
#endif

#include "dualsense/DualSenseFirmware.h"
#include "dualsense/DualSenseUsbReports.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

std::int16_t readI16(const auto& report, std::size_t offset) {
    return static_cast<std::int16_t>(report[offset] | (report[offset + 1] << 8));
}

} // namespace

int main() {
    using namespace asb::dualsense;

    // D-pad bitmask (1 up, 2 down, 4 left, 8 right) -> USB hat.
    assert(dpadMaskToHat(0x00) == 8);
    assert(dpadMaskToHat(0x01) == 0);
    assert(dpadMaskToHat(0x09) == 1);
    assert(dpadMaskToHat(0x08) == 2);
    assert(dpadMaskToHat(0x0A) == 3);
    assert(dpadMaskToHat(0x02) == 4);
    assert(dpadMaskToHat(0x06) == 5);
    assert(dpadMaskToHat(0x04) == 6);
    assert(dpadMaskToHat(0x05) == 7);
    assert(dpadMaskToHat(0x03) == 8);  // up + down cancel

    // A neutral state centres the sticks and reports no touch contacts.
    const auto neutral = encodeUsbInputReport(DualSenseInputState{}, 7, 0x01020304);
    assert(neutral.size() == kUsbInputReportSize);
    assert(neutral[0] == kUsbInputReportId);
    assert(neutral[1] == 0x80 && neutral[2] == 0x80 && neutral[3] == 0x80 && neutral[4] == 0x80);
    assert(neutral[7] == 7);
    assert(neutral[8] == 0x08);  // hat neutral, no face buttons
    assert(neutral[28] == 0x04 && neutral[31] == 0x01);
    assert((neutral[33] & 0x80) != 0 && (neutral[37] & 0x80) != 0);

    DualSenseInputState state{};
    state.lx = 0x10;
    state.r2 = 0xFF;
    state.dpad = 0x01;
    state.buttons = button::kCross | button::kTriangle | button::kR1 | button::kOptions |
                    button::kPs | button::kTouchpadClick;
    state.touch1Active = true;
    state.touch1X = 1919;
    state.touch1Y = 1079;
    state.gyroX = -1234;
    state.accelZ = 8192;
    state.batteryPercent = 70;
    state.chargeState = chargeStatus::kCharging;
    const auto report = encodeUsbInputReport(state, 0, 0);
    assert(report[1] == 0x10);
    assert(report[6] == 0xFF);
    assert(report[8] == (0xA0 | 0x00));            // cross + triangle, hat up
    assert(report[9] == ((button::kR1 | button::kOptions) >> 8));
    assert(report[10] == 0x03);                     // PS + touchpad click
    assert(readI16(report, 16) == -1234);
    assert(readI16(report, 26) == 8192);
    assert(report[33] == 0x00);                     // contact 0, touching
    const auto touchX = report[34] | ((report[35] & 0x0F) << 8);
    const auto touchY = (report[35] >> 4) | (report[36] << 4);
    assert(touchX == 1919 && touchY == 1079);
    assert(report[53] == 0x17);                     // charging, 70 %

    // Output report 0x02 as a game writes it: rumble, both trigger effects
    // and a lightbar colour.
    std::array<std::uint8_t, kUsbOutputReportSize> output{};
    output[0] = kUsbOutputReportId;
    output[1] = 0x03;          // compatible vibration + haptics select
    output[2] = 0x04;          // lightbar control enable
    output[3] = 0x40;          // right motor
    output[4] = 0x80;          // left motor
    output[11] = 0x26;         // right trigger: weapon effect
    output[12] = 0x02;
    output[22] = 0x21;         // left trigger: feedback effect
    output[39] = 0x04;         // compatible vibration 2
    output[45] = 10;
    output[46] = 20;
    output[47] = 30;
    const auto feedback = decodeUsbOutputReport(output);
    assert(feedback.has_value());
    assert(feedback->kind == FeedbackKind::HidOutput);
    assert(feedback->rumbleRight == 0x40 && feedback->rumbleLeft == 0x80);
    assert(feedback->requestsRumbleUpdate());
    assert(feedback->hasTriggerEffect());
    assert(feedback->rightTriggerEffect[0] == 0x26 && feedback->rightTriggerEffect[1] == 0x02);
    assert(feedback->leftTriggerEffect[0] == 0x21);
    assert(feedback->hasLightbarColor());
    assert(feedback->lightbarRed == 10 && feedback->lightbarGreen == 20 &&
           feedback->lightbarBlue == 30);

    output[0] = 0x31;  // Bluetooth output report: not ours
    assert(!decodeUsbOutputReport(output).has_value());
    assert(!decodeUsbOutputReport(std::span<const std::uint8_t>(output.data(), 10)).has_value());

    // Feature reports hid-playstation reads while probing.
    const auto pairing = buildPairingFeature({0x02, 0x41, 0x53, 0x42, 0x00, 0x01});
    assert(pairing[0] == kPairingFeatureId);
    assert(pairing[1] == 0x01 && pairing[6] == 0x02);  // least-significant byte first

    const auto calibration = buildCalibrationFeature();
    assert(calibration[0] == kCalibrationFeatureId);
    assert(readI16(calibration, 7) > 0 && readI16(calibration, 9) < 0);
    assert(readI16(calibration, 19) == 540);

    const auto firmware = buildFirmwareFeature();
    const auto decoded = decodeFirmwareFeatureReport(firmware);
    assert(decoded.has_value());
    assert(decoded->buildDate == "Jan 01 2026");
    assert(decoded->updateVersion >= 0x0215);

    std::cout << "DualSense USB report tests passed\n";
    return 0;
}
