#pragma once

#include "dualsense/DualSenseFeedback.h"
#include "dualsense/DualSenseInput.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace asb::dualsense {

// Wire format of a USB-attached DualSense (054C:0CE6), as consumed by the
// Linux hid-playstation driver and by games reading /dev/hidraw. Used by
// backends that expose a real HID device (Linux uhid) instead of the VIIPER
// framing.
inline constexpr std::uint16_t kSonyVendorId = 0x054C;
inline constexpr std::uint16_t kDualSenseProductId = 0x0CE6;

inline constexpr std::uint8_t kUsbInputReportId = 0x01;
inline constexpr std::uint8_t kUsbOutputReportId = 0x02;
inline constexpr std::uint8_t kCalibrationFeatureId = 0x05;
inline constexpr std::uint8_t kPairingFeatureId = 0x09;
inline constexpr std::uint8_t kFirmwareFeatureId = 0x20;

inline constexpr std::size_t kUsbInputReportSize = 64;
// As declared by the descriptor (report ID included). hid-playstation pads
// its writes to 63 bytes; the decoder accepts any length from 48 up.
inline constexpr std::size_t kUsbOutputReportSize = 48;
inline constexpr std::size_t kCalibrationFeatureSize = 41;
inline constexpr std::size_t kPairingFeatureSize = 20;
inline constexpr std::size_t kFirmwareFeatureSize = 64;

// The DualSense USB hat switch: 0 = up, clockwise to 7 = up-left, 8 = neutral.
// DualSenseInputState::dpad is a bitmask (1 up, 2 down, 4 left, 8 right).
[[nodiscard]] std::uint8_t dpadMaskToHat(std::uint8_t mask) noexcept;

// Builds input report 0x01 including the report ID byte.
[[nodiscard]] std::array<std::uint8_t, kUsbInputReportSize> encodeUsbInputReport(
    const DualSenseInputState& state,
    std::uint8_t sequence,
    std::uint32_t sensorTimestamp);

// Decodes output report 0x02 (report ID byte included) into the same
// DualSenseFeedback the VIIPER backend produces for frame type 0x01.
[[nodiscard]] std::optional<DualSenseFeedback> decodeUsbOutputReport(
    std::span<const std::uint8_t> report);

// Feature reports hid-playstation and games query when the device appears.
// The values describe a plausible retail pad; the virtual device has no
// physical IMU calibration of its own.
[[nodiscard]] std::array<std::uint8_t, kCalibrationFeatureSize> buildCalibrationFeature();
[[nodiscard]] std::array<std::uint8_t, kPairingFeatureSize> buildPairingFeature(
    const std::array<std::uint8_t, 6>& macAddress);
[[nodiscard]] std::array<std::uint8_t, kFirmwareFeatureSize> buildFirmwareFeature();

} // namespace asb::dualsense
