# ApexSenseBridge on Linux (work in progress)

This branch ports ApexSenseBridge to Linux, so Proton and native games can drive
a Flydigi APEX through a virtual DualSense the same way they do on Windows.

**Status: early port, not usable for gaming yet.** The shared engine builds and
its test suite passes on Linux; the platform layer is being filled in piece by
piece. The virtual DualSense works end to end (verified on kernel 7.3: the
kernel binds `hid-playstation`, games' trigger and rumble reports reach the
bridge). Nothing has been tested on Flydigi hardware yet.

## Why the port is mostly a platform layer

Everything that understands the hardware is already portable C++20:
the APEX 4/5/6 protocols, DualSense trigger-effect translation, rumble and
haptic processing, input mapping and gyro handling. The Windows build reaches
the OS only through the interfaces in `src/platform/` and `VirtualDualSense`,
and the non-Windows build already links stub implementations of them. A Linux
port replaces those stubs:

| Windows piece | Linux replacement | State |
|---|---|---|
| SetupAPI + HID.dll (`WindowsHidTransport`) | `hidraw` + sysfs (`linux/LinuxHidTransport.cpp`) | working on DualSense and 8BitDo; untested on APEX |
| VIIPER + USBip kernel driver (virtual DualSense) | `uhid` in user space (`linux/LinuxVirtualDualSense.cpp`) | working |
| HidHide (hide the physical pad) | udev permissions / `EVIOCGRAB` / Steam ignore lists | planned |
| XInput reading of the APEX gamepad interface | evdev or the APEX vendor input reports | planned |
| WASAPI endpoint handling for DualSense haptic audio | PipeWire virtual 4-channel sink named like a DualSense | planned |
| Named mutex / events (`WindowsSessionControl`) | lock file + Unix socket | planned |
| Tray app, Playnite integration | out of scope for now (CLI first) | — |

No kernel modules or out-of-tree drivers are needed: `hidraw` and `uhid` ship
with every distribution kernel.

### Virtual DualSense via uhid

The bridge creates a HID device with Sony's USB IDs (054C:0CE6) and the real
DualSense USB report descriptor (captured from a physical pad, see
`scripts/linux/dump-dualsense-descriptor.sh`). The kernel binds its own
`hid-playstation` driver to it, and Steam, SDL and Proton see a wired DualSense:

- input report `0x01` is built from `DualSenseInputState`
  (`src/dualsense/DualSenseUsbReports.cpp`);
- output report `0x02` from games (rumble, both adaptive-trigger effect blocks,
  lightbar) arrives as `UHID_OUTPUT` and is decoded into the same
  `DualSenseFeedback` the VIIPER backend produces, so the existing trigger,
  rumble and lightbar bridges run unchanged;
- the feature reports the driver probes (calibration `0x05`, pairing `0x09`,
  firmware `0x20`) are answered by the bridge.

Audio haptics are a separate channel on a real pad (USB audio, channels 3–4),
which uhid cannot carry. They need the PipeWire part of the roadmap. Even with
a real DualSense, Proton only delivers audio haptics in some games.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

Requirements: a C++20 compiler (GCC 13+ or Clang 17+), CMake 3.20+, kernel
headers (`linux/hidraw.h`, `linux/uhid.h`).

## Trying what exists today

Install the udev rules so the logged-in user can open the devices, then log in
again (or replug the controller):

```sh
sudo install -m644 packaging/linux/70-apexsensebridge.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

```sh
./build/ApexSenseBridge list            # APEX vendor HID interfaces via hidraw
./build/ApexSenseBridge diagnose --json # what the transport sees (please share!)
./build/ApexSenseBridge identify        # read the APEX identity
./build/ApexSenseBridge virtual-ds --seconds 60
```

While `virtual-ds` runs, a "DualSense Wireless Controller" appears
(`evtest`, Steam's controller settings, `dmesg | grep playstation`), and the
command counts the trigger and rumble reports games send to it.

## How you can help

If you own an APEX 4, 5 or 6 Pro and run Linux, open an issue with the output
of `ApexSenseBridge list` and `ApexSenseBridge diagnose --json`. Interface
numbers, usage pages and report lengths on Linux are the first thing to
confirm before the bridge loop can be enabled.

## Upstream

This is a fork of [ReynArts/ApexSenseBridge](https://github.com/ReynArts/ApexSenseBridge)
(MIT). The Linux pieces are kept behind the existing platform interfaces so
they can be proposed upstream once they work on real hardware.
