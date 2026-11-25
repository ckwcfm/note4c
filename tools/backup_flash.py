#!/usr/bin/env python3
"""Read-only NOTE4C backup preparation. Never invoked by builds or tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone

FLASH_SIZE = 16 * 1024 * 1024


def read_flash(port, output, first):
    # no-reset after each read returns to ROM download mode, so the installed
    # firmware cannot modify NVS between reads. No erase/write command exists here.
    subprocess.run([
        sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
        "--baud", "460800", "--before", "default-reset" if first else "no-reset",
        "--after", "no-reset", "read-flash", "0x0", "0x1000000", str(output),
    ], check=True)


def run_backup(port, destination, reader=read_flash):
    destination = Path(destination).expanduser().resolve()
    if any((p / ".git").exists() for p in [destination, *destination.parents]):
        raise ValueError("Backups may contain credentials; choose a destination outside Git repositories")
    destination.mkdir(mode=0o700, parents=True, exist_ok=False)
    records = []
    for index in range(2):
        path = destination / f"factory-read-{index + 1}.bin"
        with path.open("xb"):
            pass
        path.chmod(0o600)
        reader(port, path, index == 0)
        if path.stat().st_size != FLASH_SIZE:
            raise ValueError(f"Read {index + 1} is not exactly 16 MiB; backup is incomplete")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        records.append({"file": path.name, "size": FLASH_SIZE, "sha256": digest})
    if records[0]["sha256"] != records[1]["sha256"]:
        raise ValueError("Flash reads differ; keep both for diagnosis and do not flash")
    manifest = {
        "model_confirmed_by_user": "ZECTRIX NOTE4C",
        "chip": "esp32s3", "flash_start": 0, "flash_size": FLASH_SIZE,
        "created_utc": datetime.now(timezone.utc).isoformat(), "reads": records,
        "verification": "Two complete reads with identical SHA-256; restore not tested",
        "device_state": "Left in ROM download mode; manually reset to resume factory firmware",
    }
    manifest_path = destination / "manifest.json"
    with manifest_path.open("x") as handle:
        os.chmod(manifest_path, 0o600)
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, help="New private folder outside every Git repository")
    parser.add_argument("--confirmed-note4c", action="store_true")
    parser.add_argument("--allow-rom-reset", action="store_true",
                        help="Acknowledge interrupting installed firmware to enter ROM download mode")
    args = parser.parse_args()
    if not args.confirmed_note4c or not args.allow_rom_reset:
        parser.error("Confirm NOTE4C identity and acknowledge ROM reset explicitly")
    try:
        manifest = run_backup(args.port, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Backup failed: {exc}\nDo not flash. Preserve any incomplete reads for diagnosis.\n")
    print(f"Two 16 MiB reads match: SHA-256 {manifest['reads'][0]['sha256']}")
    print("Device remains in ROM download mode. No flash content was written or erased.")


if __name__ == "__main__":
    main()
