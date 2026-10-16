#!/usr/bin/env python3
"""Exercise production client/FAT/session code over real loopback TCP sockets."""
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil
build = Path(sys.argv[1] if len(sys.argv) > 1 else 'build')
for size in (2, 16):
    with tempfile.TemporaryDirectory(prefix='rntp-linux-test-') as work:
        image = Path(work) / 'disk.img'
        server = subprocess.Popen([str(build / 'server-test'), str(size), str(image)], stdout=subprocess.PIPE, text=True)
        try:
            port = server.stdout.readline().strip()
            assert port.isdigit(), 'test server failed to start'
            subprocess.run([str(build / 'client-test'), '127.0.0.1', port], check=True, timeout=90)
        finally:
            server.terminate()
            assert server.wait(timeout=10) == 0
        if shutil.which('fsck.fat'):
            subprocess.run(['fsck.fat', '-n', str(image)], check=True)
        print(f'Linux TCP/FAT{12 if size == 2 else 16}: PASS', flush=True)
