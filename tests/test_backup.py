import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("backup_flash", Path(__file__).parents[1] / "tools/backup_flash.py")
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)


class BackupTests(unittest.TestCase):
    def test_two_complete_identical_reads(self):
        calls = []
        def reader(port, path, first):
            calls.append((port, first))
            path.write_bytes(b"\x55" * backup.FLASH_SIZE)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / "private-backup"
            result = backup.run_backup("MOCK-NOT-A-DEVICE", folder, reader)
            self.assertEqual(calls, [("MOCK-NOT-A-DEVICE", True), ("MOCK-NOT-A-DEVICE", False)])
            self.assertEqual(result["reads"][0]["sha256"], result["reads"][1]["sha256"])
            self.assertEqual(json.loads((folder / "manifest.json").read_text()), result)
            self.assertEqual(folder.stat().st_mode & 0o777, 0o700)
            self.assertEqual((folder / "factory-read-1.bin").stat().st_mode & 0o777, 0o600)

    def test_short_read_never_marks_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / "short"
            with self.assertRaisesRegex(ValueError, "incomplete"):
                backup.run_backup("MOCK", folder, lambda p, f, first: f.write_bytes(b"short"))
            self.assertFalse((folder / "manifest.json").exists())

    def test_different_reads_never_mark_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / "different"
            with self.assertRaisesRegex(ValueError, "differ"):
                backup.run_backup("MOCK", folder,
                    lambda p, f, first: f.write_bytes(bytes([1 if first else 2]) * backup.FLASH_SIZE))
            self.assertFalse((folder / "manifest.json").exists())

    def test_repository_and_existing_destination_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(FileExistsError):
                backup.run_backup("MOCK", root)
            (root / ".git").mkdir()
            with self.assertRaisesRegex(ValueError, "credentials"):
                backup.run_backup("MOCK", root / "forbidden")


if __name__ == "__main__":
    unittest.main()
