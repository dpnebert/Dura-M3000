# Dura M-3000

Firmware and hardware design files for the **Dura M-3000 liquid metering and pump-control system**.

The M-3000 uses an ESP32-S3 controller with a local LCD/keypad interface and Bluetooth Low Energy (BLE) communication for the Dura app. This project uses the single-meter M3000 configuration; the networked Dura V2 / ESP-NOW configuration is a separate product scope.

## Repository layout

```text
.
├── README.md
├── firmware/                 # ESP-IDF application, configuration and tests
│   ├── main/                 # Application startup and command handling
│   ├── components/           # Board support, metering, control and BLE modules
│   ├── scripts/              # Regression checks and development/bench utilities
│   ├── tests/                # Host-side test fixtures and supporting code
│   ├── tools/                # Asset conversion utilities
│   ├── CMakeLists.txt        # ESP-IDF project definition
│   ├── sdkconfig             # Saved project configuration
│   ├── sdkconfig.defaults    # Baseline configuration defaults
│   ├── dependencies.lock     # Resolved component and ESP-IDF versions
│   └── version.txt           # Build version identifier
└── hardware/                 # Schematic and PCB design files
```

## Functionality

- Flow measurement and volume display in gallons, liters and ounces.
- Manual dispensing, automatic batching and recirculation controls.
- Pump, injection-valve and recirculation-valve output control.
- Calibration workflows and persistent meter settings.
- Local LCD/keypad operation and BLE command handling.
- Battery monitoring, backlight control and deep-sleep support.

## Hardware platform

The firmware baseline targets the **PCB832 revX3 controller**, with the **PCB790 pump/relay interface** forming part of the system hardware.

| Item | Firmware baseline / intended system |
| --- | --- |
| Module | Espressif `ESP32-S3-WROOM-1-N8R2` |
| ESP-IDF target | `esp32s3` |
| Flash | 8 MB |
| PSRAM | Disabled in the firmware configuration |
| Display | 128 × 64 LCD with keypad input |
| Communications | BLE; UART0 for programming and console access |
| System power | Nominal 12 V lead-acid battery |
| CAD workflow | DipTrace |

Use the exact schematic, PCB revision and fitted parts for the unit being assembled. The nominal battery voltage is not a specification for the complete operating or transient voltage range. Hardware changes may require corresponding GPIO, polarity or configuration changes in the firmware.

## Build the firmware

### Prerequisites

- **ESP-IDF v6.0.2**, matching the checked-in dependency lockfile and verified project build baseline.
- The ESP32-S3 toolchain and Python environment installed through ESP-IDF.
- An activated ESP-IDF terminal. On Windows, use the ESP-IDF terminal or the ESP-IDF VS Code extension's terminal.

See Espressif's [ESP32-S3 setup guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) for environment installation. Use the project version above rather than assuming the latest SDK is interchangeable.

From the repository root:

```sh
cd firmware
idf.py --version
idf.py build
```

Build output is generated under `firmware/build/`, including the application image, bootloader, partition table and debug ELF. The application image is named `dura_m3000_firmware.bin`.

The saved configuration already selects `esp32s3`, 8 MB flash and disabled PSRAM. Preserve these settings unless intentionally changing the supported hardware.

To inspect or change project configuration from `firmware/`:

```sh
idf.py menuconfig
```

Board settings are under **Dura M3000 board**. Review configuration changes against the corresponding hardware revision before building a device image.

## Program and monitor a unit

**The normal configuration enables pump and valve outputs. Make connected loads safe before flashing, resetting or running commands.** Close other applications using the serial port before programming.

From `firmware/`, replace `PORT` with the connected programmer's serial port:

```sh
idf.py -p PORT flash monitor
```

The console is configured for UART0 at 115200 baud. Exit the monitor with `Ctrl+]`.

Use the board's programming interface and its schematic for wiring. Do not apply battery voltage to ESP32 GPIO or serial signals.

When programming prebuilt binaries instead of building locally, use the bootloader, partition table, application image and flash offsets from one matched release. The ELF is for debugging, not flashing. Do not perform a full-chip erase as a routine update; it removes persistent settings and calibration data.

## Tests and development tools

Host-side regression checks are in `firmware/scripts/`, with fixtures in `firmware/tests/`. They cover areas such as configuration/GPIO mapping, meter behavior, output ownership, BLE handling, UI contracts and battery processing.

For example, from `firmware/`, run the battery-history host regression with Python 3 and a compatible host C toolchain:

```sh
python scripts/test_battery_rolling_history.py
```

Some host checks use GNU compiler/linker features; a Linux development environment or WSL is appropriate for those checks. Read a script before running it: this directory also contains utilities that communicate with hardware, not only offline tests.

Passing a host regression or compiling successfully does not establish correct physical wiring, output polarity, electrical protection or complete app/device compatibility. Verify those on the matching hardware.

## Hardware design files

The [hardware directory](hardware/) contains the design files. Maintain schematic and PCB revisions together, and associate manufacturing outputs with the revision that generated them. Component substitutions must account for electrical ratings and the actual land pattern, not just package names.

The [firmware directory](firmware/) contains the application source and its build configuration. Keep firmware changes traceable to the hardware revision they support.
