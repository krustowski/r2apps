"""Boot the supplied ROUREXS.COM and exercise its shell in both backends."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


host, window_host, image = (Path(arg).resolve() for arg in sys.argv[1:4])
keys = "600:0x23,700:0x12,800:0x26,900:0x19,1000:0x1c"
env = dict(os.environ, THEM_RUN_MS="2200", THEM_KEYS=keys)

with tempfile.TemporaryDirectory(prefix="them-r1-") as directory:
    work = Path(directory)
    shutil.copyfile(image, work / "ROUREXS.COM")
    for executable, options, label in (
        (host, [], "console"),
        (window_host, ["--host", "test"], "window"),
    ):
        result = subprocess.run(
            [str(executable), *options, "--org", "0", "ROUREXS.COM"],
            cwd=work, env=env, capture_output=True, text=True, timeout=8,
        )
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert "[guest@rourex:/]>" in output, output
        assert "show commands" in output and "system version" in output, output
        if label == "console":
            assert "RoureXOS FatTriangle3" in output and "Starting shell..." in output, output
        else:
            assert re.search(r"\[window\] frames=[1-9][0-9]* width=640 height=200 failed=0", output), output
        print(f"ROUREXS boot and help ({label}) PASS")
