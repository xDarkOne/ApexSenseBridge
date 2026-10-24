#ifdef NDEBUG
#undef NDEBUG
#endif

#include "dualsense/DualSenseUsbDescriptor.h"
#include "platform/HidReportDescriptor.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using asb::platform::parseHidReportDescriptor;

    // Vendor command channel shaped like the Flydigi/8BitDo configuration
    // interfaces: usage page 0xFF7A, numbered 63-byte input and output reports.
    constexpr std::array<std::uint8_t, 34> vendor{
        0x06, 0x7A, 0xFF,  // Usage Page (0xFF7A)
        0x09, 0x01,        // Usage (1)
        0xA1, 0x01,        // Collection (Application)
        0x85, 0x02,        //   Report ID (2)
        0x09, 0x01,        //   Usage (1)
        0x15, 0x00,        //   Logical Minimum (0)
        0x26, 0xFF, 0x00,  //   Logical Maximum (255)
        0x75, 0x08,        //   Report Size (8)
        0x95, 0x3F,        //   Report Count (63)
        0x81, 0x02,        //   Input
        0x85, 0x81,        //   Report ID (0x81)
        0x09, 0x01,        //   Usage (1)
        0x95, 0x3F,        //   Report Count (63)
        0x91, 0x02,        //   Output
        0xC0,              // End Collection
        0x00, 0x00,        // trailing padding is ignored
    };
    const auto vendorCaps = parseHidReportDescriptor(vendor);
    assert(vendorCaps.usagePage == 0xFF7A);
    assert(vendorCaps.usage == 0x0001);
    assert(vendorCaps.usesReportIds);
    assert(vendorCaps.inputReportLength == 64);
    assert(vendorCaps.outputReportLength == 64);
    assert(vendorCaps.featureReportLength == 0);

    // Unnumbered gamepad with an extended (page + usage) collection usage:
    // lengths still include the Windows report-ID byte.
    constexpr std::array<std::uint8_t, 25> gamepad{
        0x05, 0x01,                    // Usage Page (Generic Desktop)
        0x0B, 0x05, 0x00, 0x01, 0x00,  // Usage (Generic Desktop: Game Pad), 4-byte form
        0xA1, 0x01,                    // Collection (Application)
        0x05, 0x09,                    //   Usage Page (Button)
        0x19, 0x01, 0x29, 0x10,        //   Usage Minimum 1, Maximum 16
        0x15, 0x00, 0x25, 0x01,        //   Logical 0..1
        0x75, 0x01, 0x95, 0x10,        //   16 x 1 bit
        0x81, 0x02,                    //   Input
    };
    const auto gamepadCaps = parseHidReportDescriptor(gamepad);
    assert(gamepadCaps.usagePage == 0x0001);
    assert(gamepadCaps.usage == 0x0005);
    assert(!gamepadCaps.usesReportIds);
    assert(gamepadCaps.inputReportLength == 3);
    assert(gamepadCaps.outputReportLength == 0);

    // Truncated and empty descriptors never read out of bounds.
    assert(parseHidReportDescriptor({}).inputReportLength == 0);
    constexpr std::array<std::uint8_t, 2> truncated{0x26, 0xFF};
    assert(parseHidReportDescriptor(truncated).inputReportLength == 0);

    // The virtual DualSense descriptor must describe what the USB report
    // helpers produce: 64-byte input report 0x01 on a Game Pad collection.
    const auto dualSense = parseHidReportDescriptor(asb::dualsense::kDualSenseUsbReportDescriptor);
    assert(dualSense.usagePage == 0x0001);
    assert(dualSense.usage == 0x0005);
    assert(dualSense.usesReportIds);
    assert(dualSense.inputReportLength == 64);
    assert(dualSense.outputReportLength >= 48);

    std::cout << "HID report descriptor tests passed\n";
    return 0;
}
