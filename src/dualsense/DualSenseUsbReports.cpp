#include "dualsense/DualSenseUsbReports.h"

#include <algorithm>
#include <cstring>

namespace asb::dualsense {
namespace {

// Byte offsets inside the USB reports, report ID at offset 0. They follow the
// layout hid-playstation reads (struct dualsense_input_report and
// dualsense_output_report_common).
namespace in {
inline constexpr std::size_t kSticks = 1;      // lx, ly, rx, ry, l2, r2
inline constexpr std::size_t kSequence = 7;
inline constexpr std::size_t kButtons = 8;     // 4 bytes
inline constexpr std::size_t kGyro = 16;       // 3 x le16
inline constexpr std::size_t kAccel = 22;      // 3 x le16
inline constexpr std::size_t kTimestamp = 28;  // le32
inline constexpr std::size_t kTouch1 = 33;     // 4 bytes per contact
inline constexpr std::size_t kTouch2 = 37;
inline constexpr std::size_t kStatus = 53;
} // namespace in

namespace out {
inline constexpr std::size_t kValidFlag0 = 1;
inline constexpr std::size_t kValidFlag1 = 2;
inline constexpr std::size_t kMotorRight = 3;
inline constexpr std::size_t kMotorLeft = 4;
inline constexpr std::size_t kRightTrigger = 11;
inline constexpr std::size_t kLeftTrigger = 22;
inline constexpr std::size_t kValidFlag2 = 39;
inline constexpr std::size_t kLightbarRed = 45;
inline constexpr std::size_t kLightbarGreen = 46;
inline constexpr std::size_t kLightbarBlue = 47;
} // namespace out

void writeU16(std::span<std::uint8_t> report, std::size_t offset, std::uint16_t value) noexcept {
    report[offset] = static_cast<std::uint8_t>(value & 0xFF);
    report[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeI16(std::span<std::uint8_t> report, std::size_t offset, std::int16_t value) noexcept {
    writeU16(report, offset, static_cast<std::uint16_t>(value));
}

void writeU32(std::span<std::uint8_t> report, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        report[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

void writeTouch(std::span<std::uint8_t> report,
                std::size_t offset,
                std::uint8_t contactId,
                bool active,
                std::uint16_t x,
                std::uint16_t y) noexcept {
    // Bit 7 set means "no contact"; the low seven bits are the tracking ID.
    report[offset] = static_cast<std::uint8_t>((active ? 0x00 : 0x80) | (contactId & 0x7F));
    report[offset + 1] = static_cast<std::uint8_t>(x & 0xFF);
    report[offset + 2] = static_cast<std::uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    report[offset + 3] = static_cast<std::uint8_t>((y >> 4) & 0xFF);
}

std::uint8_t encodeStatus(std::uint8_t batteryPercent, std::uint8_t chargeState) noexcept {
    // Low nibble: capacity in tens of percent; high nibble: 0 discharging,
    // 1 charging, 2 full.
    const auto level = static_cast<std::uint8_t>(std::min<int>(batteryPercent, 100) / 10);
    std::uint8_t charging = 0;
    if (chargeState == chargeStatus::kCharging) {
        charging = 1;
    } else if (chargeState == chargeStatus::kFull) {
        charging = 2;
    }
    return static_cast<std::uint8_t>((charging << 4) | std::min<std::uint8_t>(level, 10));
}

} // namespace

std::uint8_t dpadMaskToHat(std::uint8_t mask) noexcept {
    const bool up = (mask & 0x01) != 0;
    const bool down = (mask & 0x02) != 0;
    const bool left = (mask & 0x04) != 0;
    const bool right = (mask & 0x08) != 0;
    // Opposite directions cancel, as on the physical hat.
    const int vertical = (up ? 1 : 0) - (down ? 1 : 0);
    const int horizontal = (right ? 1 : 0) - (left ? 1 : 0);
    if (vertical > 0 && horizontal == 0) return 0;
    if (vertical > 0 && horizontal > 0) return 1;
    if (vertical == 0 && horizontal > 0) return 2;
    if (vertical < 0 && horizontal > 0) return 3;
    if (vertical < 0 && horizontal == 0) return 4;
    if (vertical < 0 && horizontal < 0) return 5;
    if (vertical == 0 && horizontal < 0) return 6;
    if (vertical > 0 && horizontal < 0) return 7;
    return 8;
}

std::array<std::uint8_t, kUsbInputReportSize> encodeUsbInputReport(
    const DualSenseInputState& state,
    std::uint8_t sequence,
    std::uint32_t sensorTimestamp) {
    std::array<std::uint8_t, kUsbInputReportSize> report{};
    const std::span<std::uint8_t> bytes(report);
    report[0] = kUsbInputReportId;
    report[in::kSticks + 0] = state.lx;
    report[in::kSticks + 1] = state.ly;
    report[in::kSticks + 2] = state.rx;
    report[in::kSticks + 3] = state.ry;
    report[in::kSticks + 4] = state.l2;
    report[in::kSticks + 5] = state.r2;
    report[in::kSequence] = sequence;

    // DualSenseInputState::buttons already uses the USB bit positions for the
    // face buttons (byte 0, high nibble) and shoulder/stick/menu buttons
    // (byte 1); its low three bits are PS, touchpad click and mute (byte 2).
    report[in::kButtons + 0] = static_cast<std::uint8_t>(
        (state.buttons & 0xF0) | dpadMaskToHat(state.dpad));
    report[in::kButtons + 1] = static_cast<std::uint8_t>(state.buttons >> 8);
    report[in::kButtons + 2] = static_cast<std::uint8_t>(state.buttons & 0x07);

    writeI16(bytes, in::kGyro + 0, state.gyroX);
    writeI16(bytes, in::kGyro + 2, state.gyroY);
    writeI16(bytes, in::kGyro + 4, state.gyroZ);
    writeI16(bytes, in::kAccel + 0, state.accelX);
    writeI16(bytes, in::kAccel + 2, state.accelY);
    writeI16(bytes, in::kAccel + 4, state.accelZ);
    writeU32(bytes, in::kTimestamp, sensorTimestamp);

    writeTouch(bytes, in::kTouch1, 0, state.touch1Active, state.touch1X, state.touch1Y);
    writeTouch(bytes, in::kTouch2, 1, state.touch2Active, state.touch2X, state.touch2Y);

    report[in::kStatus] = encodeStatus(state.batteryPercent, state.chargeState);
    return report;
}

std::optional<DualSenseFeedback> decodeUsbOutputReport(std::span<const std::uint8_t> report) {
    // Games send the full 63-byte report; accept anything that reaches the
    // lightbar colour so truncated writes from simple tools still decode.
    if (report.size() <= out::kLightbarBlue || report[0] != kUsbOutputReportId) {
        return std::nullopt;
    }

    DualSenseFeedback feedback{};
    feedback.kind = FeedbackKind::HidOutput;
    feedback.enableBits1 = report[out::kValidFlag0];
    feedback.enableBits2 = report[out::kValidFlag1];
    feedback.rumbleRight = report[out::kMotorRight];
    feedback.rumbleLeft = report[out::kMotorLeft];
    feedback.enableBits3 = report[out::kValidFlag2];
    std::copy_n(report.begin() + out::kRightTrigger, feedback.rightTriggerEffect.size(),
                feedback.rightTriggerEffect.begin());
    std::copy_n(report.begin() + out::kLeftTrigger, feedback.leftTriggerEffect.size(),
                feedback.leftTriggerEffect.begin());
    constexpr std::uint8_t kLightbarControlEnable = 0x04;
    if ((feedback.enableBits2 & kLightbarControlEnable) != 0) {
        feedback.lightbarRed = report[out::kLightbarRed];
        feedback.lightbarGreen = report[out::kLightbarGreen];
        feedback.lightbarBlue = report[out::kLightbarBlue];
        feedback.hasLightbar = true;
    }
    return feedback;
}

std::array<std::uint8_t, kCalibrationFeatureSize> buildCalibrationFeature() {
    std::array<std::uint8_t, kCalibrationFeatureSize> report{};
    const std::span<std::uint8_t> bytes(report);
    report[0] = kCalibrationFeatureId;
    // Gyro biases (pitch, yaw, roll) stay zero. Then per axis the +/- range,
    // the rotation speed the range was measured at, and accelerometer ranges.
    constexpr std::int16_t kGyroRange = 8800;
    constexpr std::int16_t kGyroSpeed = 540;
    constexpr std::int16_t kAccelRange = 8192;
    writeI16(bytes, 7, kGyroRange);
    writeI16(bytes, 9, -kGyroRange);
    writeI16(bytes, 11, kGyroRange);
    writeI16(bytes, 13, -kGyroRange);
    writeI16(bytes, 15, kGyroRange);
    writeI16(bytes, 17, -kGyroRange);
    writeI16(bytes, 19, kGyroSpeed);
    writeI16(bytes, 21, kGyroSpeed);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        writeI16(bytes, 23 + axis * 4, kAccelRange);
        writeI16(bytes, 25 + axis * 4, -kAccelRange);
    }
    return report;
}

std::array<std::uint8_t, kPairingFeatureSize> buildPairingFeature(
    const std::array<std::uint8_t, 6>& macAddress) {
    std::array<std::uint8_t, kPairingFeatureSize> report{};
    report[0] = kPairingFeatureId;
    // The pad reports its address least-significant byte first.
    std::reverse_copy(macAddress.begin(), macAddress.end(), report.begin() + 1);
    return report;
}

std::array<std::uint8_t, kFirmwareFeatureSize> buildFirmwareFeature() {
    std::array<std::uint8_t, kFirmwareFeatureSize> report{};
    const std::span<std::uint8_t> bytes(report);
    report[0] = kFirmwareFeatureId;
    constexpr char kBuildDate[] = "Jan 01 2026";
    constexpr char kBuildTime[] = "00:00:00";
    std::memcpy(report.data() + 1, kBuildDate, 11);
    std::memcpy(report.data() + 12, kBuildTime, 8);
    writeU16(bytes, 20, 0x0004);        // firmware type
    writeU16(bytes, 22, 0x0001);        // software series
    writeU32(bytes, 24, 0x00000613);    // hardware info
    writeU32(bytes, 28, 0x0110002A);    // firmware version
    // hid-playstation switches to the second-generation vibration and player
    // LED handling from update version 0x0215 onward.
    writeU16(bytes, 44, 0x0630);
    return report;
}

} // namespace asb::dualsense
