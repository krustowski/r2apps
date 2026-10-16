#!/usr/bin/env python3
"""Mount two FUSE clients on temporary directories and exercise Linux file I/O."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time
base = Path(__file__).resolve().parents[1]

def exercise(a, b):
    (a / 'DATA').mkdir()
    data = bytes((i * 37 + 19) & 255 for i in range(131073))
    (a / 'DATA' / 'IMAGE.BIN').write_bytes(data)
    assert (b / 'data' / 'image.bin').read_bytes() == data
    with (b / 'DATA' / 'IMAGE.BIN').open('r+b') as f:
        f.seek(509)
        f.write(b'foreign-change')
        f.truncate(8000)
    expected = data[:509] + b'foreign-change' + data[523:8000]
    assert (a / 'DATA' / 'IMAGE.BIN').read_bytes() == expected
    with (a / 'DATA' / 'IMAGE.BIN').open('r+b') as f:
        f.seek(16000)
        f.write(b'after-gap')
    assert (b / 'DATA' / 'IMAGE.BIN').read_bytes() == expected + bytes(8000) + b'after-gap'
    os.rename(b / 'DATA' / 'IMAGE.BIN', b / 'CHANGED.BIN')
    assert not (a / 'DATA' / 'IMAGE.BIN').exists()
    assert (a / 'CHANGED.BIN').stat().st_size == 16009
    (a / 'APPEND.TXT').touch()
    def writer(mount, index):
        directory = mount / ('USER' + str(index)); directory.mkdir()
        for i in range(24):
            payload = bytes([index + 70]) * (4000 + i)
            (directory / ('F' + str(i) + '.BIN')).write_bytes(payload)
            assert (directory / ('F' + str(i) + '.BIN')).read_bytes() == payload
            fd = os.open(mount / 'APPEND.TXT', os.O_WRONLY | os.O_APPEND)
            try:
                assert os.write(fd, bytes([index + 40]) * 128) == 128
            finally:
                os.close(fd)
    with ThreadPoolExecutor(2) as pool:
        futures = [pool.submit(writer, a, 0), pool.submit(writer, b, 1)]
        for f in futures: f.result(timeout=60)
    appended = (b / 'APPEND.TXT').read_bytes()
    assert len(appended) == 24 * 2 * 128
    assert appended.count(40) == appended.count(41) == 24 * 128
    assert len(list((b / 'USER0').iterdir())) == 24
    assert len(list((a / 'USER1').iterdir())) == 24
    assert os.statvfs(a).f_bfree == os.statvfs(b).f_bfree
    (b / 'CHANGED.BIN').unlink()
    assert not (a / 'CHANGED.BIN').exists()
    (b / 'DATA').rmdir()
    assert not (a / 'DATA').exists()
    print('two mounted Linux clients: file I/O, sparse gaps, remote rename/delete, concurrent append and directory growth PASS', flush=True)

if __name__ == '__main__':
    build = Path(sys.argv[1] if len(sys.argv) > 1 else base / 'build').resolve()
    fuse = Path(sys.argv[2] if len(sys.argv) > 2 else base / 'rntp-fuse').resolve()
    for size in (2, 16):
        with tempfile.TemporaryDirectory(prefix='rntp-fuse-test-') as work:
            work = Path(work); image = work / 'disk.img'
            server = subprocess.Popen([str(build / 'server-test'), str(size), str(image)], stdout=subprocess.PIPE, text=True)
            mounts, processes, logs = [], [], []
            try:
                port = server.stdout.readline().strip(); assert port.isdigit()
                for name in ('a', 'b'):
                    mount = work / name; mount.mkdir(); mounts.append(mount)
                    log = (work / (name + '.log')).open('w+'); logs.append(log)
                    p = subprocess.Popen([str(fuse), '127.0.0.1', str(mount), '-p', port, '-f'], stdout=log, stderr=log)
                    processes.append(p)
                    deadline = time.monotonic() + 10
                    while not os.path.ismount(mount):
                        if p.poll() is not None or time.monotonic() > deadline:
                            log.seek(0); raise AssertionError('FUSE mount failed: ' + log.read())
                        time.sleep(.05)
                exercise(*mounts)
            finally:
                for mount in reversed(mounts):
                    if os.path.ismount(mount): subprocess.run(['fusermount3', '-u', str(mount)], check=True)
                for p in processes:
                    try: p.wait(timeout=10)
                    except subprocess.TimeoutExpired: p.terminate(); p.wait(timeout=10)
                for log in logs: log.close()
                server.terminate(); assert server.wait(timeout=10) == 0
            if shutil.which('fsck.fat'): subprocess.run(['fsck.fat', '-n', str(image)], check=True)
            print(f'Actual Linux FUSE FAT{12 if size == 2 else 16} mounts: PASS', flush=True)
