#include "dualsense/DualSenseUsbDescriptor.h"
#include "dualsense/DualSenseUsbReports.h"
#include "dualsense/VirtualDualSense.h"

#include <linux/input.h>
#include <linux/uhid.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace asb::dualsense {
namespace {

std::string errnoText(int code) {
    return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ')';
}

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

// Locally administered unicast address, so the virtual pad never collides
// with a real Sony MAC and Steam keeps a stable identity across sessions.
constexpr std::array<std::uint8_t, 6> kVirtualMac{0x02, 0x41, 0x53, 0x42, 0x00, 0x01};

class LinuxVirtualDualSense final : public VirtualDualSense {
public:
    explicit LinuxVirtualDualSense(VirtualDualSenseOptions options) : options_(std::move(options)) {}

    ~LinuxVirtualDualSense() override { close(); }

    bool open(std::string& error, FeedbackHandler handler) override {
        if (fd_ >= 0) {
            return true;
        }
        const auto started = std::chrono::steady_clock::now();
        handler_ = std::move(handler);

        fd_ = ::open("/dev/uhid", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            const int code = errno;
            error = "Opening /dev/uhid failed: " + errnoText(code);
            if (code == EACCES) {
                error += ". Install packaging/linux/70-apexsensebridge.rules (gives the "
                         "logged-in user access to /dev/uhid) and log in again.";
            } else if (code == ENOENT) {
                error += ". Load the uhid kernel module: sudo modprobe uhid";
            }
            return false;
        }

        uhid_event event{};
        event.type = UHID_CREATE2;
        auto& create = event.u.create2;
        std::snprintf(reinterpret_cast<char*>(create.name), sizeof(create.name), "%s",
                      "Sony Interactive Entertainment DualSense Wireless Controller");
        std::snprintf(reinterpret_cast<char*>(create.phys), sizeof(create.phys), "%s",
                      "apexsensebridge/virtual-dualsense");
        std::snprintf(reinterpret_cast<char*>(create.uniq), sizeof(create.uniq),
                      "%02x:%02x:%02x:%02x:%02x:%02x", kVirtualMac[0], kVirtualMac[1],
                      kVirtualMac[2], kVirtualMac[3], kVirtualMac[4], kVirtualMac[5]);
        create.bus = BUS_USB;
        create.vendor = kSonyVendorId;
        create.product = kDualSenseProductId;
        create.version = 0x0100;
        create.country = 0;
        create.rd_size = static_cast<std::uint16_t>(kDualSenseUsbReportDescriptor.size());
        std::memcpy(create.rd_data, kDualSenseUsbReportDescriptor.data(),
                    kDualSenseUsbReportDescriptor.size());
        if (!writeEvent(event, error)) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        stopping_.store(false);
        reader_ = std::thread([this] { readLoop(); });
        {
            std::lock_guard lock(statsMutex_);
            stats_.connected = true;
            stats_.backendVersion = "linux-uhid";
            stats_.initializationDeviceUs = elapsedUs(started);
        }

        // Publish one neutral report so hid-playstation and games start from
        // centred sticks instead of an all-zero report (sticks pinned up-left).
        return updateInput(DualSenseInputState{}, error);
    }

    void close() noexcept override {
        if (fd_ < 0) {
            return;
        }
        stopping_.store(true);
        uhid_event event{};
        event.type = UHID_DESTROY;
        std::string ignored;
        writeEvent(event, ignored);
        if (reader_.joinable()) {
            reader_.join();
        }
        ::close(fd_);
        fd_ = -1;
        std::lock_guard lock(statsMutex_);
        stats_.connected = false;
    }

    bool updateInput(const DualSenseInputState& state, std::string& error) override {
        if (fd_ < 0) {
            error = "The virtual DualSense is not open";
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        // The sensor clock counts in units of 0.33 us.
        const auto timestamp = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                started.time_since_epoch()).count() / 333);
        const auto report = encodeUsbInputReport(state, sequence_++, timestamp);

        uhid_event event{};
        event.type = UHID_INPUT2;
        event.u.input2.size = static_cast<std::uint16_t>(report.size());
        std::memcpy(event.u.input2.data, report.data(), report.size());
        if (!writeEvent(event, error)) {
            return false;
        }

        const auto duration = elapsedUs(started);
        std::lock_guard lock(statsMutex_);
        ++stats_.inputUpdates;
        stats_.maxInputUpdateDurationUs = std::max(stats_.maxInputUpdateDurationUs, duration);
        return true;
    }

    bool connected() const noexcept override { return fd_ >= 0; }

    VirtualDualSenseStats stats() const override {
        std::lock_guard lock(statsMutex_);
        return stats_;
    }

private:
    bool writeEvent(const uhid_event& event, std::string& error) {
        std::lock_guard lock(writeMutex_);
        const auto written = ::write(fd_, &event, sizeof(event));
        if (written != static_cast<ssize_t>(sizeof(event))) {
            error = "Writing to /dev/uhid failed: " +
                    (written < 0 ? errnoText(errno) : std::string("short write"));
            return false;
        }
        return true;
    }

    void readLoop() {
        while (!stopping_.load()) {
            pollfd descriptor{fd_, POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, 100);
            if (ready <= 0) {
                continue;
            }
            uhid_event event{};
            const auto count = ::read(fd_, &event, sizeof(event));
            if (count <= 0) {
                continue;
            }
            switch (event.type) {
            case UHID_OUTPUT:
                handleOutput(event.u.output);
                break;
            case UHID_GET_REPORT:
                answerGetReport(event.u.get_report);
                break;
            case UHID_SET_REPORT:
                answerSetReport(event.u.set_report);
                break;
            default:
                // UHID_START/STOP/OPEN/CLOSE need no action: input reports are
                // accepted whether or not a reader has the device open.
                break;
            }
        }
    }

    void handleOutput(const uhid_output_req& output) {
        const std::span<const std::uint8_t> report(output.data, output.size);
        const auto feedback = decodeUsbOutputReport(report);
        {
            std::lock_guard lock(statsMutex_);
            ++stats_.outputReports;
            if (!feedback) {
                ++stats_.unknownFrames;
            } else {
                if (feedback->hasTriggerEffect()) ++stats_.triggerReports;
                if (feedback->requestsRumbleUpdate()) ++stats_.rumbleReports;
            }
        }
        if (feedback && handler_) {
            handler_(*feedback);
        }
    }

    void answerGetReport(const uhid_get_report_req& request) {
        uhid_event reply{};
        reply.type = UHID_GET_REPORT_REPLY;
        auto& answer = reply.u.get_report_reply;
        answer.id = request.id;

        const auto copy = [&answer](const auto& data) {
            answer.size = static_cast<std::uint16_t>(data.size());
            std::memcpy(answer.data, data.data(), data.size());
        };
        if (request.rtype != UHID_FEATURE_REPORT) {
            answer.err = EIO;
        } else if (request.rnum == kCalibrationFeatureId) {
            copy(buildCalibrationFeature());
        } else if (request.rnum == kPairingFeatureId) {
            copy(buildPairingFeature(kVirtualMac));
        } else if (request.rnum == kFirmwareFeatureId) {
            copy(buildFirmwareFeature());
        } else {
            // Other Sony feature reports (factory test, Edge profiles) are not
            // emulated; a game probing them gets an I/O error like on a pad
            // that rejects the request.
            answer.err = EIO;
        }
        std::string ignored;
        writeEvent(reply, ignored);
    }

    void answerSetReport(const uhid_set_report_req& request) {
        uhid_event reply{};
        reply.type = UHID_SET_REPORT_REPLY;
        reply.u.set_report_reply.id = request.id;
        reply.u.set_report_reply.err = 0;
        std::string ignored;
        writeEvent(reply, ignored);
    }

    VirtualDualSenseOptions options_;
    FeedbackHandler handler_;
    int fd_ = -1;
    std::uint8_t sequence_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread reader_;
    std::mutex writeMutex_;
    mutable std::mutex statsMutex_;
    VirtualDualSenseStats stats_;
};

} // namespace

std::unique_ptr<VirtualDualSense> createVirtualDualSense(VirtualDualSenseOptions options) {
    return std::make_unique<LinuxVirtualDualSense>(std::move(options));
}

} // namespace asb::dualsense
