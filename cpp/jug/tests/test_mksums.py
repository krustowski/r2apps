import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "mksums.py"


class ManifestTests(unittest.TestCase):
    def test_nested_sizes_checksums_and_atomic_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            (root / "bin" / "TNT.ELF").write_bytes(b"tnt")
            (root / "sh.elf").write_bytes(b"shell")
            (root / "readme.txt").write_text("ignored")
            output = root / "sums.txt"
            command = [sys.executable, str(SCRIPT), str(root), "--output", str(output),
                       "--updated", "2026-10-08 12:00:00 UTC"]
            subprocess.run(command, check=True)
            text = output.read_text()
            self.assertIn("# updated 2026-10-08 12:00:00 UTC", text)
            self.assertIn(f"{hashlib.sha256(b'tnt').hexdigest()}  3  bin/TNT.ELF", text)
            self.assertIn(f"{hashlib.sha256(b'shell').hexdigest()}  5  sh.elf", text)
            self.assertNotIn("readme", text)
            (root / "tnt.elf").write_bytes(b"duplicate")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("duplicate program tnt", result.stderr)
            self.assertEqual(output.read_text(), text)

    def test_names_and_empty_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = [sys.executable, str(SCRIPT), str(root)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            (root / "toolongname.elf").write_bytes(b"invalid name")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
