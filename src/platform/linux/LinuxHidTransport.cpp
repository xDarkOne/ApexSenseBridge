#include "platform/HidReportDescriptor.h"
#include "platform/HidTransport.h"
#include "platform/linux/LinuxText.h"

#include <linux/hidraw.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace asb::platform {
namespace {

namespace fs = std::filesystem;

std::string readTrimmed(const fs::path& path) {
    std::ifstream stream(path);
    std::string value;
    std::getline(stream, value);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

std::vector<std::uint8_t> readBinary(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string ueventValue(const fs::path& ueventPath, std::string_view key) {
    std::ifstream stream(ueventPath);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 &&
            line[key.size()] == '=') {
            return line.substr(key.size() + 1);
        }
    }
    return {};
}

// HID_ID=0003:000037D7:00002502 -> bus, vendor, product.
bool parseHidId(const std::string& hidId, std::uint16_t& vendorId, std::uint16_t& productId) {
    unsigned bus = 0;
    unsigned vendor = 0;
    unsigned product = 0;
    if (std::sscanf(hidId.c_str(), "%x:%x:%x", &bus, &vendor, &product) != 3) {
        return false;
    }
    vendorId = static_cast<std::uint16_t>(vendor);
    productId = static_cast<std::uint16_t>(product);
    return true;
}

std::string errnoText(int code) {
    return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ')';
}

// Returns the closest ancestor of `start` that contains `marker`.
std::optional<fs::path> ancestorWith(const fs::path& start, std::string_view marker) {
    for (auto current = start; !current.empty() && current != current.root_path();
         current = current.parent_path()) {
        std::error_code ec;
        if (fs::exists(current / marker, ec)) {
            return current;
        }
    }
    return std::nullopt;
}

class LinuxHidTransport final : public HidTransport {
public:
    LinuxHidTransport(HidDeviceInfo info, int fd, bool usesReportIds)
        : info_(std::move(info)), fd_(fd), usesReportIds_(usesReportIds) {}

    ~LinuxHidTransport() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    LinuxHidTransport(const LinuxHidTransport&) = delete;
    LinuxHidTransport& operator=(const LinuxHidTransport&) = delete;

    bool isOpen() const noexcept override { return fd_ >= 0; }
    const HidDeviceInfo& info() const noexcept override { return info_; }

    bool writeOutputReport(std::span<const std::uint8_t> report, std::string& error) override {
        if (!usable(error)) {
            return false;
        }
        if (info_.outputReportLength == 0) {
            error = "This HID interface declares no output report";
            return false;
        }
        if (report.empty() || report.size() > info_.outputReportLength) {
            std::ostringstream oss;
            oss << "Protocol report is " << report.size()
                << " bytes, but HID output report length is only " << info_.outputReportLength;
            error = oss.str();
            return false;
        }
        // Same buffer convention as Windows WriteFile: byte 0 is the report ID
        // (0 for unnumbered reports, which hidraw strips), padded to the
        // declared output length.
        std::vector<std::uint8_t> buffer(info_.outputReportLength, 0);
        std::copy(report.begin(), report.end(), buffer.begin());
        const auto written = ::write(fd_, buffer.data(), buffer.size());
        if (written < 0) {
            error = "write to " + narrow(info_.path) + " failed: " + errnoText(errno);
            return false;
        }
        return true;
    }

    bool readFeatureReport(std::span<std::uint8_t> report, std::string& error) override {
        if (!usable(error)) {
            return false;
        }
        if (report.empty() || report.size() > 4096) {
            error = "HID feature-report buffer has an invalid size";
            return false;
        }
        // report[0] carries the requested report ID, as with IOCTL_HID_GET_FEATURE.
        const auto result = ::ioctl(fd_, HIDIOCGFEATURE(report.size()), report.data());
        if (result < 0) {
            error = "Reading the HID feature report failed: " + errnoText(errno);
            return false;
        }
        return true;
    }

    HidReadStatus readInputReport(std::span<std::uint8_t> report,
                                  std::chrono::milliseconds timeout,
                                  std::size_t& bytesRead,
                                  std::string& error) override {
        bytesRead = 0;
        if (!usable(error)) {
            return HidReadStatus::Error;
        }
        if (report.size() < 2) {
            error = "HID input buffer is too small";
            return HidReadStatus::Error;
        }

        pollfd descriptor{fd_, POLLIN, 0};
        const auto waitMs = static_cast<int>(std::clamp<long long>(timeout.count(), 0, 0x7FFFFFFF));
        const auto ready = ::poll(&descriptor, 1, waitMs);
        if (ready < 0) {
            if (errno == EINTR) {
                return HidReadStatus::Timeout;
            }
            error = "poll on " + narrow(info_.path) + " failed: " + errnoText(errno);
            return HidReadStatus::Error;
        }
        if (ready == 0) {
            return HidReadStatus::Timeout;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            error = narrow(info_.path) + " was disconnected";
            return HidReadStatus::Error;
        }

        // hidraw only prefixes the report ID for numbered reports; Windows
        // always does (0 when unnumbered), and the protocol code expects that.
        const auto target = usesReportIds_ ? report : report.subspan(1);
        const auto count = ::read(fd_, target.data(), target.size());
        if (count < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return HidReadStatus::Timeout;
            }
            error = "read from " + narrow(info_.path) + " failed: " + errnoText(errno);
            return HidReadStatus::Error;
        }
        if (count == 0) {
            return HidReadStatus::Timeout;
        }
        if (!usesReportIds_) {
            report[0] = 0;
        }
        bytesRead = static_cast<std::size_t>(count) + (usesReportIds_ ? 0 : 1);
        return HidReadStatus::Data;
    }

private:
    bool usable(std::string& error) const {
        if (fd_ < 0) {
            error = "HID handle is not open";
            return false;
        }
        return true;
    }

    HidDeviceInfo info_;
    int fd_ = -1;
    bool usesReportIds_ = false;
};

std::optional<HidDeviceInfo> describeHidraw(const fs::path& classEntry, bool& usesReportIds) {
    std::error_code ec;
    const auto hidDevice = fs::canonical(classEntry / "device", ec);
    if (ec) {
        return std::nullopt;
    }

    HidDeviceInfo info{};
    info.path = widen("/dev/" + classEntry.filename().string());
    info.instanceId = widen(hidDevice.string());
    info.className = L"HIDClass";

    const auto uevent = hidDevice / "uevent";
    if (!parseHidId(ueventValue(uevent, "HID_ID"), info.vendorId, info.productId)) {
        return std::nullopt;
    }
    info.product = widen(ueventValue(uevent, "HID_NAME"));
    info.friendlyName = info.product;
    info.serial = widen(ueventValue(uevent, "HID_UNIQ"));

    const auto caps = parseHidReportDescriptor(readBinary(hidDevice / "report_descriptor"));
    info.usagePage = caps.usagePage;
    info.usage = caps.usage;
    info.inputReportLength = caps.inputReportLength;
    info.outputReportLength = caps.outputReportLength;
    info.featureReportLength = caps.featureReportLength;
    usesReportIds = caps.usesReportIds;

    // USB parents: the interface directory has bInterfaceNumber, the device
    // directory above it has idVendor and the string descriptors. The device
    // directory plays the role of the Windows container ID: every interface
    // of one physical controller shares it.
    if (const auto interfaceDir = ancestorWith(hidDevice.parent_path(), "bInterfaceNumber")) {
        info.parentInstanceId = widen(interfaceDir->string());
        const auto number = readTrimmed(*interfaceDir / "bInterfaceNumber");
        if (number.size() == 2) {
            info.interfaceNumber = widen("MI_" + number);
        }
        if (const auto usbDevice = ancestorWith(interfaceDir->parent_path(), "idVendor")) {
            info.containerId = widen(usbDevice->string());
            if (const auto manufacturer = readTrimmed(*usbDevice / "manufacturer");
                !manufacturer.empty()) {
                info.manufacturer = widen(manufacturer);
            }
            if (const auto product = readTrimmed(*usbDevice / "product"); !product.empty()) {
                info.product = widen(product);
            }
            if (info.serial.empty()) {
                info.serial = widen(readTrimmed(*usbDevice / "serial"));
            }
        }
    }

    char vidPid[32];
    std::snprintf(vidPid, sizeof(vidPid), "HID\\VID_%04X&PID_%04X", info.vendorId, info.productId);
    info.hardwareIds.push_back(widen(vidPid));
    return info;
}

} // namespace

std::vector<HidDeviceInfo> enumerateHidDevices(std::string& error, HidStringQuery strings) {
    (void)strings;  // sysfs strings are free to read on Linux.
    std::vector<HidDeviceInfo> devices;
    std::error_code ec;
    const fs::path classDir("/sys/class/hidraw");
    if (!fs::exists(classDir, ec)) {
        error = "/sys/class/hidraw does not exist; is the hidraw driver loaded?";
        return devices;
    }
    for (const auto& entry : fs::directory_iterator(classDir, ec)) {
        bool usesReportIds = false;
        if (auto info = describeHidraw(entry.path(), usesReportIds)) {
            devices.push_back(std::move(*info));
        }
    }
    std::sort(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    return devices;
}

HidTransport* createHidTransport(const HidDeviceInfo& info, std::string& error) {
    const auto path = narrow(info.path);
    const auto name = std::filesystem::path(path).filename();
    bool usesReportIds = false;
    // Re-read the descriptor: the caller's HidDeviceInfo carries no report-ID flag.
    if (!describeHidraw(std::filesystem::path("/sys/class/hidraw") / name, usesReportIds)) {
        error = path + " is not a hidraw device";
        return nullptr;
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        const int code = errno;
        error = "Opening " + path + " failed: " + errnoText(code);
        if (code == EACCES) {
            error += ". Install packaging/linux/70-apexsensebridge.rules so the "
                     "logged-in user can open Flydigi hidraw nodes.";
        }
        return nullptr;
    }
    return new LinuxHidTransport(info, fd, usesReportIds);
}

void destroyHidTransport(HidTransport* transport) noexcept {
    delete transport;
}

} // namespace asb::platform
