#!/usr/bin/env bash
# Regenerates src/dualsense/DualSenseUsbDescriptor.h from a real DualSense
# (054C:0CE6) connected over USB. The descriptor is the pad's own interface
# description, read from sysfs; no third-party emulator data is involved.
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
output="$repo_root/src/dualsense/DualSenseUsbDescriptor.h"

descriptor=
for node in /sys/class/hidraw/hidraw*; do
    if grep -qi '^HID_ID=0003:0000054C:00000CE6$' "$node/device/uevent" 2>/dev/null; then
        descriptor="$node/device/report_descriptor"
        break
    fi
done
if [[ -z $descriptor ]]; then
    echo "No USB DualSense (054C:0CE6) found. Connect one by cable and retry." >&2
    exit 1
fi

# sysfs reports a 4096-byte size for this attribute; count the real bytes.
size=$(wc -c < "$descriptor")
{
    echo '#pragma once'
    echo
    echo '#include <array>'
    echo '#include <cstdint>'
    echo
    echo 'namespace asb::dualsense {'
    echo
    echo '// USB HID report descriptor of a DualSense (054C:0CE6), captured from a'
    echo '// physical pad with scripts/linux/dump-dualsense-descriptor.sh.'
    echo "inline constexpr std::array<std::uint8_t, $size> kDualSenseUsbReportDescriptor{"
    od -An -v -tx1 -w12 "$descriptor" | sed -E 's/ ([0-9a-f]{2})/ 0x\1,/g; s/^ */    /; s/,$/,/'
    echo '};'
    echo
    echo '} // namespace asb::dualsense'
} > "$output"
echo "Wrote $output ($size bytes)"
