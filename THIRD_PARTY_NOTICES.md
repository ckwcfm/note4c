# Third-party provenance

The NOTE4C SSD2683 protocol sequence in `components/note4c_core/src/ssd2683.cpp`
and board mapping in `firmware/main/board.hpp` were adapted from
[LazyYoun/youn-ink-fourcolor-firmware](https://github.com/LazyYoun/youn-ink-fourcolor-firmware),
branch `2bp`, revision `8907e59300fde3b77a385dba9f3070b0de17b4c8`.

Source paths:

- `firmware/main/boards/zectrix-s3-epaper-4.2/custom_lcd_display.cc`
- `firmware/main/boards/zectrix-s3-epaper-4.2/config.h`
- `firmware/main/boards/zectrix-s3-epaper-4.2/board_power_bsp.cc`

Both upstream MIT notices are retained verbatim:

- [Repository notice](third_party/licenses/reference-root-MIT.txt): copyright 2026 macheng2017.
- [Firmware notice](third_party/licenses/reference-firmware-MIT.txt): copyright 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and Project Contributors.

Include these notices when redistributing the adapted implementation. MIT permits
commercial use and modification; it does not require publishing modifications.
The new framework code is MIT under the root `LICENSE`.

RawDraw, its LVGL-derived font compatibility layer, widget implementations, and
upstream font assets are **not vendored**. The example renderer and compact bitmap
alphabet are implemented here. Adding RawDraw/fonts later requires retaining their
applicable licenses and confirming the provenance of each selected font asset;
a repository-level MIT label alone is not an asset audit.

ESP-IDF is an external dependency. Its components carry their own notices, including
Apache-2.0 code and vendor binary-library licenses. The two upstream MIT notices
do not relicense ESP-IDF. Consult the pinned SDK's license files when distributing
firmware. No managed third-party components are required by this baseline.
