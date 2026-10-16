#!/usr/bin/env python3
"""Build an isolated boot image and run the network ABI test on both NICs."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def run(command, **kwargs):
    subprocess.run(command, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path, help="built text kernel ELF")
    args = parser.parse_args()
    kernel = args.kernel.resolve(strict=True)
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="r2-network-") as directory:
        root = Path(directory)
        build = root / "app"
        run(["make", "-C", str(tests), f"BUILD={build}", "build"])
        boot = root / "iso" / "boot"
        (boot / "grub").mkdir(parents=True)
        shutil.copyfile(kernel, boot / "kernel.elf")
        (boot / "grub" / "grub.cfg").write_text(
            'set timeout=0\nset default=0\nmenuentry "Network ABI test" {\n'
            ' multiboot2 /boot/kernel.elf\n boot\n}\n'
        )
        iso = root / "smoke.iso"
        with (root / "iso-build.log").open("w") as log:
            run(["grub2-mkrescue", "-o", str(iso), str(root / "iso")], stdout=log, stderr=log)
        init = root / "INIT.RC"
        init.write_text("cd /mnt/fat\nfg NETMON\n")
        for model in ("rtl8139", "e1000"):
            fat = root / f"{model}.img"
            with fat.open("wb") as image:
                image.truncate(1440 * 1024)
            run(["mformat", "-i", str(fat), "::"])
            run(["mcopy", "-i", str(fat), str(build / "NETMON.ELF"), "::NETMON.ELF"])
            run(["mcopy", "-i", str(fat), str(init), "::INIT.RC"])
            debug = root / f"{model}.log"
            command = [
                "qemu-system-x86_64", "-boot", "d", "-m", "256M", "-cdrom", str(iso),
                "-drive", f"file={fat},format=raw,if=floppy", "-nic", f"user,model={model}",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-debugcon", f"file:{debug}", "-serial", "none", "-display", "none",
                "-monitor", "none", "-no-reboot", "-no-shutdown",
            ]
            with (root / f"{model}-qemu.log").open("w") as output:
                vm = subprocess.Popen(command, stdout=output, stderr=output)
                try:
                    deadline = time.monotonic() + 30
                    while True:
                        result = debug.read_text() if debug.exists() else ""
                        if "NETWORK ABI FAIL" in result:
                            raise RuntimeError(f"{model}: {result.strip()}")
                        if "NETWORK ABI PASS" in result:
                            print(f"{model}: NETWORK ABI PASS", flush=True)
                            break
                        if vm.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError(f"{model}: test did not finish; {result.strip()}")
                        time.sleep(0.1)
                finally:
                    if vm.poll() is None:
                        vm.terminate()
                    try:
                        vm.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        vm.kill()
                        vm.wait()


if __name__ == "__main__":
    main()
