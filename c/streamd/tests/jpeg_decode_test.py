#!/usr/bin/env python3
"""Decode both encoder profiles with Pillow, independently of TinyJPEG."""
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, JpegImagePlugin

root = Path(__file__).resolve().parents[1]
env = dict(os.environ, CCACHE_DISABLE="1")
with tempfile.TemporaryDirectory(prefix="streamd-jpeg-") as directory:
    temp = Path(directory)
    for sampling in (444, 420):
        fixtures = temp / str(sampling)
        fixtures.mkdir()
        executable = fixtures / "encode"
        subprocess.run([
            "gcc", "-O3", "-ffreestanding", f"-DTJE_SUBSAMPLING={sampling}",
            "-Dmemcpy=r2_test_memcpy", "-Dmemcmp=r2_test_memcmp",
            f"-I{root}", f"-I{root.parent / 'libcr2'}",
            str(root / "tests/jpeg_encode_test.c"), str(root.parent / "libcr2/mem.c"),
            "-o", str(executable),
        ], check=True, env=env)
        subprocess.run([str(executable), str(fixtures)], check=True)
        for test, size in enumerate(((1, 1), (17, 19), (31, 9), (32, 32), (640, 480), (17, 19))):
            with Image.open(fixtures / f"{test}.jpg") as picture:
                assert picture.size == size, (sampling, test, picture.size)
                assert JpegImagePlugin.get_sampling(picture) == (0 if sampling == 444 else 2)
                picture.load()
                if test in (0, 1, 2, 5):
                    assert all(max(abs(a - b) for a, b in zip(pixel, (93, 157, 211))) <= 5
                               for pixel in picture.getdata()), (sampling, test, "colour/edge")
                if test == 3:
                    for point, expected in [((8, 8), (255, 0, 0)), ((24, 8), (0, 255, 0)),
                                            ((8, 24), (0, 0, 255)), ((24, 24), (255, 255, 255))]:
                        assert max(abs(a - b) for a, b in zip(picture.getpixel(point), expected)) <= 8
        assert (fixtures / "1.jpg").read_bytes() == (fixtures / "5.jpg").read_bytes(), "alpha changed JPEG"
print("JPEG decoding: both sampling modes, edge padding, colours and alpha passed")
