# Hardware work: separate approved milestone

No hardware has been flashed, reset, backed up or otherwise accessed during this
implementation. A connected device is not authorization to write it. Confirm the
physical model is NOTE4C and the panel is four-color; USB chip identification alone
cannot distinguish it from a monochrome NOTE4.

## Complete factory backup before any approved flashing

First agree on interrupting the factory firmware to enter ROM download mode, identify
the exact serial port, and choose a private backup location outside any repository
or shared project folder. The full flash can contain Wi-Fi passwords and device
secrets. Store it privately and retain a second protected copy.

The supplied helper is **read-only with respect to flash**, but resets the device
and loads esptool's temporary RAM stub. It is not run by build, simulator, CI or tests.
Use the activated ESP-IDF Python environment containing esptool 5.x. After approval
for the backup/reset operation, invoke from the repository root, substituting the
confirmed port and a new private directory:

```sh
python tools/backup_flash.py \
  --port /dev/cu.usbmodemREPLACE \
  --output /absolute/private/location/note4c-factory-YYYYMMDD \
  --confirmed-note4c --allow-rom-reset
```

The helper reads offset `0x000000` through `0xFFFFFF` twice, each exactly **16,777,216
bytes**, leaves the chip in ROM download mode between reads, and requires identical
SHA-256 hashes. It writes a manifest only on success. It refuses existing destinations
and paths inside Git repositories. Output directories/files use 0700/0600 permissions.
The helper contains no write-flash, erase, partition-changing or eFuse command.

If automatic entry fails, agree on manual BOOT/RESET entry using the vendor guide.
Do not use erase or write as a connection diagnostic. An interrupted, short or
mismatched read is not a verified backup. Keep the evidence and investigate. The
helper leaves the device in ROM mode; manually RESET after the read-only work is
finished to resume factory firmware.

Two matching reads verify byte consistency, not a recovery procedure. Record device
model/PCB, factory version if known, flash geometry, USB identity, tool version,
backup hashes and security state. If secure boot or flash encryption is enabled,
understand recovery constraints before proceeding; raw flash does not back up eFuses
or provide encryption/signing keys. Do not modify those features.

## Review a concrete deployment plan

Before requesting approval to flash:

1. Verify both complete backups and preserve the manifest separately.
2. Decode the factory partition table offline from the full dump (commonly at
   `0x8000`, confirm rather than assume), and inspect app headers/security settings.
3. Compare factory regions with the build's `partition-table.bin` and `flasher_args.json`.
   The current build uses IDF's stock single large app table solely to produce a
   compileable artifact; it is **not approved for the connected device**. If factory
   offsets and app sizes allow it, prefer an approved app-only deployment preserving
   factory layout. Otherwise prepare a separate proposed layout for explicit approval.
4. Record the exact candidate app/bootloader/table hashes, write offsets and restore
   strategy, and identify which regions would change. Do not assume a merged image
   or `idf.py flash` preserves the installed table or NVS.
5. Ask for explicit approval of those concrete writes. Approval to back up is not
   approval to flash, erase, repartition or restore.

No flashing/erase helper is supplied. Do not run ESP-IDF's automatically printed
flashing suggestions before this review. No monochrome firmware may be installed.

## Hardware validation after separately approved deployment

Check latch behavior on USB and battery, startup-held buttons, panel power and BUSY
polarity, orientation and all four colors. Measure full-refresh timing and verify
the final value after rapid input and navigation during refresh. Confirm LED fault
feedback and recovery with controlled conditions; never disconnect the panel live.
Verify counter persistence without touching other namespaces. Test runtime Wi-Fi
credentials on a trusted 2.4 GHz network, without logging or committing credentials.

Only then consider optional deep sleep: validate GPIO0/GPIO18 wake, released-button
handling, GPIO17 retention, charging inhibition and actual current draw. Keep it
disabled until tested. Audio rails remain off; microphone/speaker, RTC alarm setup,
battery calibration and NFC are separate bring-up tasks. Update `VALIDATION.md`
with observed results, device revision and firmware hash.
