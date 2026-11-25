# Validation record

Date: 2026-11-14. Hardware operations: **none**. There is no physical-device test
result or factory backup yet. This is a compile-checked development foundation.

## Checks performed

- macOS arm64, AppleClang 21.0.0: Debug host build with AddressSanitizer and
  UndefinedBehaviorSanitizer. CTest passes seven C++ suites covering packed color
  bytes/clipping, deterministic rendering, lifecycle and navigation, busy-display
  input, latest-frame coalescing, deduplication, cooldown, starvation bound, faults,
  retry, startup-held buttons, debounce/long presses, persistence/schema decoding, sleep blockers,
  and panel command/error sequencing.
- Panel tests inject a failure at **every write** and every BUSY wait in the
  sequence, assert immediate abort, and check that power is removed on every failure.
  These test software behavior, not electrical panel safety or waveform quality.
- Python backup-helper tests pass four cases: matching complete reads, incomplete
  read, mismatched reads, and unsafe/existing destination rejection. Readers are
  mocked; no test opens a serial device.
- ESP-IDF v6.0 (6.0.0), SDK commit `662a3be354759d9487bf4b1a629fadb766cb1800`,
  Xtensa ESP32-S3 build: successful app, bootloader, partition artifact and size check.
  Default app size: `0xB9550` bytes (759,120), fitting the **build candidate**
  `0x177000` app partition with 51% free. This is not a factory-layout compatibility check.
- The optional deep-sleep branch also compiles using a separate temporary SDK
  configuration and `build-sleep` directory. Its generated configuration enables
  `CONFIG_NOTE4C_ENABLE_DEEP_SLEEP`; the default configuration remains disabled.
- SDK was installed temporarily at `/tmp/note4c-idf-v6.0` with tools at
  `/tmp/note4c-idf-tools`. These temporary paths are not project dependencies and may
  disappear. Install a persistent SDK separately for routine development.

The build emitted upstream SDK Kconfig parser/deprecation notes. Project compilation
completed successfully. GitHub Actions is configured for Linux host tests and a
v6.0 firmware build; no remote CI run has occurred and the repository has not been
pushed or committed.

## Remaining practical limits

- Device identity, display protocol, timing, full-color output, pin levels, battery
  latch, USB behavior and PSRAM operation require physical validation.
- Four-color partial refresh is unverified and not exposed. Full-screen updates
  exceed ten seconds according to the vendor; the simulator's 12 seconds is an
  illustrative duration, not a measured device result.
- NVS and Wi-Fi adapters compile; real persistence, association, disconnection and
  event ordering are not device-tested. Wi-Fi defaults off; no credential provisioning,
  background client, backoff strategy or OTA is included.
- Deep sleep defaults off and needs hardware qualification before enabling. RTC
  reads have basic checks; there is no alarm setup, calendar validation, or time-set
  UI. Battery output is raw ADC, not a calibrated voltage or percentage.
- Audio and NFC are extension designs, not complete drivers. The minimal renderer
  handles a compact ASCII alphabet; multilingual fonts and more complex layouts
  can be added behind the rendering boundary.
- Input and lifecycle code are deterministic host-tested logic; mechanical switches,
  task timing and factory button/power interactions still need validation.
