#include "platform/HidReportDescriptor.h"

#include <algorithm>
#include <array>
#include <vector>

namespace asb::platform {
namespace {

enum class ItemType : std::uint8_t { Main = 0, Global = 1, Local = 2, Reserved = 3 };

struct GlobalState {
    std::uint16_t usagePage = 0;
    std::uint32_t reportSize = 0;
    std::uint32_t reportCount = 0;
    std::uint8_t reportId = 0;
};

// Bits per report, indexed by report ID, for one report kind.
using ReportBits = std::array<std::uint32_t, 256>;

std::uint16_t lengthFromBits(const ReportBits& bits) noexcept {
    const auto longest = *std::max_element(bits.begin(), bits.end());
    if (longest == 0) {
        return 0;
    }
    const auto bytes = (longest + 7) / 8 + 1;
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(bytes, 0xFFFF));
}

} // namespace

HidReportDescriptorCaps parseHidReportDescriptor(
    std::span<const std::uint8_t> descriptor) noexcept {
    HidReportDescriptorCaps caps{};
    GlobalState global{};
    std::vector<GlobalState> stack;
    ReportBits inputBits{};
    ReportBits outputBits{};
    ReportBits featureBits{};
    std::uint32_t lastUsage = 0;
    bool lastUsageHasPage = false;
    int collectionDepth = 0;
    bool topLevelFound = false;

    std::size_t offset = 0;
    while (offset < descriptor.size()) {
        const auto prefix = descriptor[offset];
        if (prefix == 0xFE) {
            // Long item: data size is in the next byte. Never used by
            // controllers, but skip it correctly.
            if (offset + 1 >= descriptor.size()) {
                break;
            }
            offset += 3 + descriptor[offset + 1];
            continue;
        }

        const std::size_t sizeCode = prefix & 0x03;
        const std::size_t dataSize = sizeCode == 3 ? 4 : sizeCode;
        if (offset + 1 + dataSize > descriptor.size()) {
            break;
        }
        std::uint32_t data = 0;
        for (std::size_t index = 0; index < dataSize; ++index) {
            data |= static_cast<std::uint32_t>(descriptor[offset + 1 + index]) << (8 * index);
        }
        const auto type = static_cast<ItemType>((prefix >> 2) & 0x03);
        const auto tag = static_cast<std::uint8_t>(prefix >> 4);
        offset += 1 + dataSize;

        switch (type) {
        case ItemType::Global:
            switch (tag) {
            case 0x0: global.usagePage = static_cast<std::uint16_t>(data); break;
            case 0x7: global.reportSize = data; break;
            case 0x8:
                global.reportId = static_cast<std::uint8_t>(data);
                caps.usesReportIds = true;
                break;
            case 0x9: global.reportCount = data; break;
            case 0xA: stack.push_back(global); break;
            case 0xB:
                if (!stack.empty()) {
                    global = stack.back();
                    stack.pop_back();
                }
                break;
            default: break;
            }
            break;
        case ItemType::Local:
            if (tag == 0x0) {
                lastUsage = data;
                lastUsageHasPage = dataSize == 4;
            }
            break;
        case ItemType::Main: {
            const auto bits = global.reportSize * global.reportCount;
            switch (tag) {
            case 0x8: inputBits[global.reportId] += bits; break;
            case 0x9: outputBits[global.reportId] += bits; break;
            case 0xB: featureBits[global.reportId] += bits; break;
            case 0xA:
                // Windows exposes one HID collection per top-level application
                // collection; the first one describes this interface.
                if (collectionDepth == 0 && !topLevelFound) {
                    caps.usagePage = lastUsageHasPage
                        ? static_cast<std::uint16_t>(lastUsage >> 16)
                        : global.usagePage;
                    caps.usage = static_cast<std::uint16_t>(lastUsage & 0xFFFF);
                    topLevelFound = true;
                }
                ++collectionDepth;
                break;
            case 0xC:
                collectionDepth = std::max(0, collectionDepth - 1);
                break;
            default: break;
            }
            // Local items only apply to the main item that follows them.
            lastUsage = 0;
            lastUsageHasPage = false;
            break;
        }
        case ItemType::Reserved:
            break;
        }
    }

    caps.inputReportLength = lengthFromBits(inputBits);
    caps.outputReportLength = lengthFromBits(outputBits);
    caps.featureReportLength = lengthFromBits(featureBits);
    return caps;
}

} // namespace asb::platform
