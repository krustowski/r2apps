import struct
import unittest

from append_manifest import SIZE, SIGNATURE, attach, trailer


def executable():
    image = bytearray(256)
    image[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHI", image, 16, 2, 62, 1)
    struct.pack_into("<Q", image, 32, 64)
    struct.pack_into("<HHH", image, 52, 64, 56, 1)
    struct.pack_into("<IIQQQQQQ", image, 64, 1, 5, 128, 0x600000, 0x600000, 128, 128, 4096)
    return bytes(image)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.manifest = dict(magic=0x31484f47, version=1, width=480, height=240, flags=2, title="Hello / Go")

    def test_layout_and_loaded_bytes(self):
        original = executable()
        image = attach(original, self.manifest)
        self.assertEqual(image[:-SIZE], original)
        self.assertEqual(len(image), len(original) + SIZE)
        metadata = struct.unpack("<16s7I80sI", image[-SIZE:])
        self.assertEqual(metadata[:8], (SIGNATURE, 1, SIZE, 0x31484f47, 1, 480, 240, 2))
        self.assertEqual(metadata[8].split(b"\0", 1)[0], b"Hello / Go")
        self.assertEqual(metadata[9], 0)
        self.assertEqual(attach(image, self.manifest), image)

    def test_invalid_manifest(self):
        for key, value in [("magic", 0), ("version", 2), ("width", 0), ("height", 2049),
                           ("flags", 4), ("title", ""), ("title", "x"*80), ("title", "line\n"),
                           ("title", "nul\0"), ("width", 1.5)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                trailer(dict(self.manifest, **{key: value}))

    def test_invalid_executable(self):
        for image in [b"", b"not ELF"*20, executable()[:120], executable()[:-1]]:
            with self.assertRaises(ValueError):
                attach(image, self.manifest)
        # An apparent trailer inside a PT_LOAD segment must not be stripped.
        image = executable()[:-SIZE] + trailer(self.manifest)
        with self.assertRaises(ValueError):
            attach(image, self.manifest)


if __name__ == "__main__":
    unittest.main()
