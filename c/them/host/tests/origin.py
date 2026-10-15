"""Exercise the CLI and loader with real assembled guests and memory dumps."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


host = Path(sys.argv[1]).resolve()
source = Path(__file__).with_suffix(".asm")
env = dict(os.environ, THEM_RUN_MS="1000")

with tempfile.TemporaryDirectory(prefix="them-org-") as directory:
    work = Path(directory)

    def run(*args):
        result = subprocess.run(
            [str(host), *args], cwd=work, env=env, capture_output=True,
            text=True, timeout=5,
        )
        return result, result.stdout + result.stderr

    for origin, options, name, tail in (
        (0x100, [], "check.com", []),
        (0x100, ["--org", "0x100"], "check.com", ["--org", "0"]),
        (0, ["--org", "0x000"], "check.com", []),
        (0, ["--org", "0"], "check", []),
        (0x200, ["--org", "512"], "check.com", []),
        (0x200, ["--org", "0X200"], "check.com", []),
    ):
        image = work / "check.com"
        subprocess.run(
            ["nasm", "-f", "bin", f"-DTEST_ORG={origin}", str(source),
             "-o", str(image)], check=True,
        )
        result, output = run(*options, name, *tail)
        assert result.returncode == 0 and "origin regression PASS" in output, output
        memory = (work / "guest.bin").read_bytes()
        data = image.read_bytes()
        base = 0x10000 + origin
        assert memory[base:base + len(data)] == data, f"image corrupted at ORG {origin:#x}"
        assert memory[:4] == struct.pack("<HH", 0x1000, 0xF000), "IVT overwritten"
        if origin >= 0x100:
            assert memory[0x1002C:0x1002E] == struct.pack("<H", 0x0F00)
            command = " ".join(tail).encode()
            assert memory[0x10080:0x10082 + len(command)] == bytes([len(command)]) + command + b"\r"

    # Relocatable executables still use their header's CS:IP with --org set.
    code = bytes.fromhex("b8 00 4c cd 21")  # INT 21h/4Ch: exit 0
    header = struct.pack("<14H", 0x5A4D, 32 + len(code), 1, 0, 2, 0, 0,
                         0, 0xFFFE, 0, 0, 0, 0x1C, 0) + bytes(4)
    (work / "check.exe").write_bytes(header + code)
    result, output = run("--org", "0x200", "check.exe", "log")
    assert result.returncode == 0, output
    log = (work / "THEM.LOG").read_text()
    assert "cs=1010 ip=0" in log and "guest exited, code 0" in log, log
    assert (work / "guest.bin").read_bytes()[0x10100:0x10100 + len(code)] == code

    for value in ("", "0x", "garbage", "-1", "+1", "1g", "65536", "0x10000",
                  "184467440737095516160"):
        result, output = run("--org", value, "check.com")
        assert result.returncode == 1 and "--org requires an offset" in output, output
    result, output = run("--org")
    assert result.returncode == 1 and "--org requires an offset" in output, output
    for value in ("0xFF00", "0xFFFF"):
        result, output = run("--org", value, "check.com")
        assert result.returncode == 1 and "cannot load" in output, output

print("origin CLI and loader regression PASS")
