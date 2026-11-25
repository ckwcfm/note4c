# Reference audit and decisions

Reviewed 2026-11-14. Sources were read before implementation. The reference was
inspected in a separate clean temporary clone; no existing project was modified.
The new repository had no prior Git history or applicable ancestor AGENTS.md.

## Sources and SDK

- [Vendor NOTE4C guide](https://wiki.zectrix.com/en/hardware/note4c/quick-start)
- [Reference branch](https://github.com/LazyYoun/youn-ink-fourcolor-firmware/tree/2bp)
- Pinned inspection revision: `8907e59300fde3b77a385dba9f3070b0de17b4c8`
- [Reference manifest](https://github.com/LazyYoun/youn-ink-fourcolor-firmware/blob/8907e59300fde3b77a385dba9f3070b0de17b4c8/firmware/main/idf_component.yml)
- [Reference lock](https://github.com/LazyYoun/youn-ink-fourcolor-firmware/blob/8907e59300fde3b77a385dba9f3070b0de17b4c8/firmware/dependencies.lock)
- [Espressif stable guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)

The upstream manifest declares IDF `>=5.4.0`; its lock records **6.0.0**, and the
README uses a 6.0 environment. Its CMake adds a C++ math compatibility include,
and the manifest explicitly adds cJSON/MQTT for IDF 6 compatibility. These are
evidence of a concrete build baseline, not proof that arbitrary newer versions
work. The current stable Espressif page identified **6.1.1** at review time.

This framework therefore starts with SDK tag **v6.0**, commit
`662a3be354759d9487bf4b1a629fadb766cb1800`, and guards the 6.0.x family. The portable
core requires C++17; the firmware uses IDF's current GPIO, SPI, I2C-master, ADC,
NVS and Wi-Fi APIs. There are no external managed components. An upgrade should
change the guard and CI SDK together, run the host suite, compile both sleep
configurations and revalidate hardware before becoming the supported baseline.

## Existing architecture

The upstream project combines firmware with Python services and management
interfaces. The firmware application owns UI/network/voice behavior and routes
board button callbacks into a RawDraw UI manager. The manifest also resolves
many unrelated display/board, LVGL, speech, codec, image and other dependencies.

RawDraw is a custom CPU raster library, not a panel driver. Its primitives operate
on packed 2-bpp pixels (`00` black, `01` white, `10` yellow, `11` red), and its theme
and widgets provide semantic styling. The primitives can operate without an LVGL
runtime, but the text layer uses LVGL-derived font types. The framebuffer wrapper
uses FreeRTOS mutexes. The UI manager includes the concrete board display header,
constructs many named page renderers and routes their input. Some comments still
say 1-bpp; the actual code and color encoding are the evidence used here.

`CustomLcdDisplay` combines GPIO/SPI, pixel storage, frame comparison, refresh
timing, worker state and legacy monochrome paths. It already includes useful ideas:
a transmission snapshot, byte/pixel comparison, a 3-second color debounce and a
display worker. There are additional refresh helpers in `ui/epd_refresh` and
`rawdraw/refresh`; their partial counters do not establish color-panel capabilities.

## Partial refresh finding

In `custom_lcd_display.cc`, `refresh_task_loop()` unconditionally sets `should_full`
for `IsFourColorPanel()`. `EPD_Display()` sends the complete 30,000-byte 2-bpp frame.
The alternative `EPD_DisplayPart()` converts old/new pixels to monochrome data.
It is not a four-color partial-update implementation.

The vendor documents full refreshes exceeding ten seconds and does not link a
manufacturer-hosted SSD2683 datasheet establishing an appropriate partial waveform.
Therefore **four-color partial refresh is not verified and is disabled**. This
conclusion is source-level verification of the reference's behavior; it is not a
claim that every SSD2683 panel can never support partial updates. Enabling a future
capability needs the exact panel revision, manufacturer instructions, temperature
constraints, waveform/color limitations and physical quality/endurance validation.

## Extend or extract?

| Choice | Advantage | Ongoing cost |
| --- | --- | --- |
| Extend upstream firmware | Existing rich pages, fonts, voice and services are immediately available | Coupled page manager, broad dependencies and product-specific lifecycle; upstream merges can affect apps and hardware together |
| Small independent framework | Clear contracts, deterministic tests, fewer components and no server requirement | Must track selected upstream driver changes and add optional features deliberately |

**Decision: small independent framework.** Adapt the NOTE4C pin map and four-color
panel sequence, implement a small portable renderer and event/runtime layer, and
leave a boundary for selective RawDraw adoption later. This avoids mechanically
copying page ownership or multiple competing refresh schedulers. Deliberate driver
changes include error propagation instead of continuing after BUSY timeout, immutable
job ownership, and no monochrome/partial branch.

Upstream and firmware MIT notices are both retained in `third_party/licenses`;
see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). There is no requirement to
publish modifications under MIT. Do retain attribution in distributions and review
SDK/vendor binary terms separately. No upstream fonts or RawDraw code were copied.

Maintain provenance with the pinned revision and source checksums in
`third_party/upstream.json`. When updating: diff these paths at the candidate
revision, describe protocol/pin changes, retain notices, rerun fault/sequence tests,
compile firmware, then validate on hardware with the approved backup/deployment
procedure. Do not automatically merge the upstream partition table, OTA settings,
service URLs, default AP passwords, or unrelated dependency manifest.

## Additional API references

- [IDF v6.0 SPI ownership and DMA](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/peripherals/spi_master.html)
- [IDF v6.0 NVS](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/storage/nvs_flash.html)
- [IDF v6.0 sleep modes](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/system/sleep_modes.html)
- [PCF8563 datasheet](https://www.nxp.com/docs/en/data-sheet/PCF8563.pdf)
