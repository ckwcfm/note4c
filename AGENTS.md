# NOTE4C framework working rules

- This repository is for the four-color ZECTRIX NOTE4C only. Do not add monochrome
  NOTE4 images, alternate monochrome panel modes, or unvalidated partial waveforms.
- Inspect Git state before editing. Keep app models and scheduling portable.
- Run host tests for core changes and an ESP-IDF build for firmware changes.
- Never flash hardware, erase storage, change device partitions, or write eFuses
  without the user's explicit approval for that action. Before approved flashing,
  obtain and verify a complete 16 MiB factory-flash backup outside Git.
- Backup scripts may reset the device; do not run them without agreement to the
  interruption. Builds and tests must never open a serial device.
- Never commit credentials, passwords, flash backups, private configuration,
  firmware build output, or logs. Never automatically erase NVS on init failure.
- Keep unrelated repositories, especially PCPL, untouched.
