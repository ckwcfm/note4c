# NOTE4C Calendar & Application Framework

ESP-IDF v6.0 / C++17 firmware and host-testable core for the **ZECTRIX NOTE4C PCB v1.0**
(ESP32-S3 N16R8, SSD2683 400 × 300 four-color e-paper: Black, White, Red, Yellow).

> [!IMPORTANT]
> This repository is for the four-color **ZECTRIX NOTE4C** only. Monochrome NOTE4
> firmware is incompatible. Read [hardware safety](docs/HARDWARE_SAFETY.md) before
> performing any flashing or backup operation.

## Features

- **Configurable 1–7 day four-color calendar view:** Choose 1 to 7 days ahead
  (default 3 days) with dynamic event-row allocation, compact inline-empty layout
  for 6–7 day views, color-coded time pills, footer calendar legend, Wi-Fi status
  icon, and calibrated battery percentage + icon.
- **Direct `.ics` / `webcal://` subscriptions:** Subscribe to up to 8 iCal feeds
  (Google Calendar secret iCal address, Apple iCloud, Outlook, TeamSnap, Nextcloud)
  and pick **Black**, **Red**, or **Yellow** for each calendar.
- **Streaming RFC 5545 parser:** Handles line unfolding, `VTIMEZONE` & IANA/POSIX
  timezones, `RRULE` recurrences (`DAILY`, `WEEKLY`, `MONTHLY`, `YEARLY`),
  `EXDATE` exclusions, `RECURRENCE-ID` overrides, and multi-day all-day events.
- **Built-in Web Management UI:**
  - First boot (or **Hold Down**) starts the `NOTE4C-Setup` hotspot (`http://192.168.4.1`)
    with captive-portal redirect and Wi-Fi scanning.
  - Shows the device's **Wi-Fi STA MAC address** on both the e-paper setup screen
    and the web page (with a one-click Copy button).
  - Once on Wi-Fi, the web page stays reachable even while the 3-day calendar is
    displayed on screen.
- **Non-blocking background sync & flexible scheduling:** Wi-Fi, SNTP + PCF8563 RTC
  sync, and HTTPS `.ics` fetches run on a dedicated background task so buttons and
  the web UI stay responsive. Configure sync frequency (30 min to once daily) and
  anchor time in the web UI.
- **Battery-first deep sleep:** Holds the battery power latch (`GPIO17`), wakes on
  schedule or **Confirm** / **Down** button press, and sleeps automatically on battery.
- **Wireless OTA updates:** Automatic GitHub Release update check & notification
  banner in the web management UI, one-click update from the web page or directly
  from the on-device Manage screen (`Up`), plus manual `.bin` upload into the
  inactive A/B OTA slot (`ota_0` ↔ `ota_1`) with ESP32-S3 + project header and
  image hash verification.

## Controls

- **Confirm (short):** On calendar view, sync calendars now; on Manage screen,
  exit back to the 3-day calendar without forcing a sync.
- **Confirm (hold 3 s, then release):** Restart the device cleanly (LED blinks
  rapidly once armed; also available via **Restart Device** in the web UI).
- **Down (short):** Open the Manage screen (shows LAN IP, Wi-Fi MAC, current
  firmware version & slot, and automatically checks GitHub for updates).
- **Down (long):** Force the `NOTE4C-Setup` Wi-Fi setup hotspot.
- **Up (long):** On Manage screen, start on-device GitHub OTA update with live
  on-screen progress bar; otherwise retry e-paper refresh after a display fault.

## Run host tests & simulator (no hardware required)

Requirements: a C++17 compiler, CMake 3.20+, Make or Ninja, and Python 3.10+.

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug -DNOTE4C_SANITIZERS=ON
cmake --build build-host --parallel 4
ctest --test-dir build-host --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
./build-host/note4c_cal_preview .
```

## Build firmware (ESP-IDF v6.0)

Install **ESP-IDF v6.0** (6.0.x) and the ESP32-S3 toolchain:

```sh
git clone --branch v6.0 --depth 1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0
cd ~/esp/esp-idf-v6.0
./install.sh esp32s3
. ./export.sh
```

Then compile (does not touch connected hardware):

```sh
cd firmware
idf.py -DIDF_TARGET=esp32s3 build
```

Output binary: `firmware/build/note4c_framework.bin`.

Before flashing a new device over USB, take and verify a full 16 MiB factory flash
backup with [tools/backup_flash.py](tools/backup_flash.py) and follow
[docs/HARDWARE_SAFETY.md](docs/HARDWARE_SAFETY.md). Once running this firmware on
the factory dual-OTA layout (`ota_0` @ `0x20000`, `ota_1` @ `0x410000`), future
updates can be uploaded wirelessly from the browser management page.

## Documentation

- [Architecture and rendering policy](docs/ARCHITECTURE.md)
- [Factory backup and hardware validation procedure](docs/HARDWARE_SAFETY.md)
- [Reference audit, SDK decision, licensing and maintenance](docs/REFERENCE_AUDIT.md)
- [Checks performed and remaining limitations](docs/VALIDATION.md)
