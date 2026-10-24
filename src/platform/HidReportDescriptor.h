#pragma once

#include <cstdint>
#include <span>

namespace asb::platform {

// What Windows reports through HidP_GetCaps, derived from a raw report
// descriptor. Lengths follow the Windows convention: the longest report of
// each kind in bytes, plus one byte for the report ID (present even when the
// descriptor declares no report IDs).
struct HidReportDescriptorCaps {
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
    std::uint16_t inputReportLength = 0;
    std::uint16_t outputReportLength = 0;
    std::uint16_t featureReportLength = 0;
    bool usesReportIds = false;
};

[[nodiscard]] HidReportDescriptorCaps parseHidReportDescriptor(
    std::span<const std::uint8_t> descriptor) noexcept;

} // namespace asb::platform
