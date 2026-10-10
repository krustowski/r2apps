#!/usr/bin/env python3
"""Boot a scratch r2 image and test against the official userspace WireGuard peer.

No host routes, root, TAP, TUN or installed configuration is needed.
Prerequisites: built wgd/eth/chat, a kernel with kernel/r2.patch, the reference
binary built from tests/reference.go in a wireguard-go checkout, QEMU, GRUB,
mtools, and Python cryptography. Test keys stay in the scratch directory.
"""
from pathlib import Path
import argparse, base64, json, os, re, shutil, signal, socket, struct, subprocess, tarfile, time
from urllib.request import urlopen
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PrivateFormat, PublicFormat, NoEncryption

p = argparse.ArgumentParser()
p.add_argument("--kernel", type=Path, required=True)
p.add_argument("--reference", type=Path, required=True)
p.add_argument("--workdir", type=Path)
p.add_argument("--no-rng", action="store_true")
p.add_argument("--cpu", default="max", help="QEMU CPU model; --no-rng disables RDSEED/RDRAND")
p.add_argument("--long", action="store_true")
p.add_argument("--port", type=int, default=51830)
p.add_argument("--routed", action="store_true", help="reference is tests/router.go; exercise hosted shell")
p.add_argument("--config", type=Path, help="test this real configuration through the hosted shell")
args = p.parse_args()
app = Path(__file__).resolve().parents[1]
root = (args.workdir or app / "build/qemu").resolve()
root.mkdir(parents=True, exist_ok=True)
os.umask(0o077)
iso = root / "iso"
(iso / "boot/grub").mkdir(parents=True, exist_ok=True)
shutil.copy2(args.kernel, iso / "boot/kernel.elf")
payload = root / "payload"
(payload / "bin").mkdir(parents=True, exist_ok=True)
for name in ("eth", "chat", "wgd"):
    shutil.copy2(app.parent / name / f"{name}.elf", payload / "bin" / f"{name}.elf")
if args.routed or args.config:
    shutil.copy2(app.parent / "r2sh/r2sh.elf", payload / "bin/sh.elf")
    shutil.copy2(app / "build/hosted.elf", payload / "bin/hosted.elf")
with tarfile.open(iso / "boot/usb.tar", "w", format=tarfile.USTAR_FORMAT) as tar:
    for file in sorted(payload.rglob("*")):
        tar.add(file, arcname="./" + str(file.relative_to(payload)), recursive=False)
(iso / "boot/grub/grub.cfg").write_text('set timeout=0\nset default=0\nmenuentry "wgd test" {\n multiboot2 /boot/kernel.elf\n module2 /boot/usb.tar usb\n boot\n}\n')
with (root / "grub.log").open("wb") as log:
    subprocess.run(["grub2-mkrescue", "-o", str(root / "r2.iso"), str(iso)], stdout=log, stderr=log, check=True)

keys = [X25519PrivateKey.generate(), X25519PrivateKey.generate()]
private = [k.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption()) for k in keys]
public = [k.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) for k in keys]
psk = os.urandom(32)
def b64(b): return base64.b64encode(b).decode()
if args.routed or args.config:
    cfg = f"[Interface]\nPrivateKey={b64(private[0])}\nAddress=10.3.255.253/24\nListenPort=51800\n[Peer]\nPublicKey={b64(public[1])}\nPresharedKey={b64(psk)}\nEndpoint=10.3.4.1:{args.port+2}\nAllowedIPs=10.3.255.0/24, 10.4.5.128/25, 10.4.6.0/24\nPersistentKeepalive=30\n"
    (root / "WGD.CFG").write_text(args.config.read_text() if args.config else cfg)
    (root / "BAD.CFG").write_text("[Interface]\nAddress=10.0.0.1/33\n")
    ready_wait = 10000 if args.no_rng else 500
    (root / "TEST.BSH").write_text(f"bg wgd /mnt/tmp/BAD.CFG\nsleep 100\nbg wgd /mnt/tmp/WGD.CFG\nsleep {ready_wait}\nping 10.4.6.68\ntraceroute 10.4.6.68\nsleep 5500\nread /mnt/tmp/WGD.LOG\necho __WGD_TEST_DONE__\n")
    (root / "INIT.RC").write_text("bg eth --ip 10.3.4.2\ncd /mnt/fat\nbg hosted\n")
    (root / "peer.json").write_text(json.dumps(dict(Private=private[1].hex(), Public=public[0].hex(), Preshared=psk.hex(), Port=args.port+2)))
else:
    (root / "WGD.CFG").write_text(f"[Interface]\nPrivateKey={b64(private[0])}\nAddress=10.77.0.1/32\nListenPort=51820\n[Peer]\nPublicKey={b64(public[1])}\nPresharedKey={b64(psk)}\nAllowedIPs=10.77.0.2/32\n")
    (root / "peer.json").write_text(json.dumps(dict(Private=private[1].hex(), Public=public[0].hex(), Preshared=psk.hex(), Endpoint=f"127.0.0.1:{args.port}")))
    (root / "INIT.RC").write_text("bg eth --ip 10.3.4.2\nbg chat s eth\ncd /mnt/fat\nbg wgd WGD.CFG\n")
floppy = root / "fat.img"
subprocess.run(["mformat", "-C", "-f", "1440", "-i", str(floppy), "::"], check=True)
for name in (("INIT.RC", "WGD.CFG", "BAD.CFG", "TEST.BSH") if args.routed or args.config else ("INIT.RC", "WGD.CFG")):
    subprocess.run(["mcopy", "-o", "-i", str(floppy), str(root / name), "::" + name], check=True)

debug = root / "debug.log"; debug.write_text("")
peer = None
peer_log = (root / "reference.log").open("wb") if args.routed and not args.config else None
if peer_log:
    peer = subprocess.Popen([str(args.reference.resolve()), str(root / "peer.json")], stdout=peer_log, stderr=peer_log)
    deadline=time.monotonic()+10
    while "ROUTER READY" not in (root / "reference.log").read_text():
        if peer.poll() is not None or time.monotonic()>deadline: raise SystemExit("router failed to start")
        time.sleep(0.05)
cpu = args.cpu + ",rdrand=off,rdseed=off" if args.no_rng else args.cpu
command = ["qemu-system-x86_64", "-boot", "d", "-m", "2G", "-cpu", cpu, "-display", "none", "-no-reboot",
           "-cdrom", str(root / "r2.iso"), "-drive", f"file={floppy},format=raw,if=floppy",
           "-serial", f"file:{root / 'serial.log'}", "-debugcon", f"file:{debug}",
           "-global", "isa-debugcon.iobase=0xe9", "-monitor", f"unix:{root / 'monitor.sock'},server=on,wait=off",
           "-netdev", f"user,id=n0,net=10.3.4.0/24,host=10.3.4.1,hostfwd=udp:127.0.0.1:{args.port}-10.3.4.2:51820,hostfwd=tcp:127.0.0.1:{args.port+1}-10.3.4.2:8080",
           "-device", "rtl8139,netdev=n0", "-object", f"filter-dump,id=dump,netdev=n0,file={root / 'network.pcap'}"]
with (root / "qemu.log").open("wb") as log:
    qemu = subprocess.Popen(command, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + (120 if args.routed or args.config or args.no_rng else 30)
        while time.monotonic() < deadline:
            text = debug.read_text()
            if (args.routed or args.config) and "HOSTED SHELL COMPLETE" in text:
                print(text)
                if "4/4 replies" not in text or " reached" not in text: raise SystemExit("routed ping/traceroute failed")
                if "\0" in text: raise SystemExit("diagnostic log contains NUL padding")
                if not re.search(r"handshakes=[1-9].*session=1",text): raise SystemExit("missing updated tunnel counters")
                if args.no_rng and "entropy: Jitterentropy" not in text: raise SystemExit("software entropy fallback not used")
                if "Address must be one IPv4" not in text: raise SystemExit("startup error was not visible in hosted shell")
                if args.routed and not args.config and "10.4.6.1" not in text: raise SystemExit("missing intermediate ICMP hop")
                print("PASS: hosted shell startup diagnostics, routed ping and ICMP traceroute")
                break
            if "no usable entropy source" in text:
                raise SystemExit(f"entropy startup failed; inspect {debug}")
            if "listening; public key follows" in text and not (args.routed or args.config):
                if args.no_rng and "entropy: Jitterentropy" not in text: raise SystemExit("software entropy fallback not used")
                with urlopen(f"http://127.0.0.1:{args.port+1}/", timeout=15) as response:
                    if response.status != 200 or b"</html>" not in response.read():
                        raise SystemExit("ordinary Ethernet HTTP regression")
                print("PASS: ordinary Ethernet HTTP still works")
                env = os.environ.copy()
                if args.long: env["WGD_LONG_TEST"] = "1"
                result = subprocess.run([str(args.reference.resolve()), str(root / "peer.json")], env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=190 if args.long else 60)
                (root / "reference.log").write_text(result.stdout + result.stderr)
                print(result.stdout, end="")
                if result.returncode:
                    print(result.stderr, end="")
                    raise SystemExit(f"reference peer failed; inspect {root}")
                break
            if qemu.poll() is not None: raise SystemExit(f"QEMU exited; inspect {root / 'qemu.log'}")
            time.sleep(0.1)
        else: raise SystemExit(f"daemon did not reach expected startup state; inspect {root}")
    finally:
        if qemu.poll() is None:
            try:
                with socket.socket(socket.AF_UNIX) as monitor:
                    monitor.settimeout(2); monitor.connect(str(root / "monitor.sock")); monitor.recv(4096)
                    monitor.sendall(f"screendump {root / 'screen.ppm'}\n".encode()); time.sleep(0.2)
            except OSError:
                pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill(); qemu.wait()
        if peer:
            peer.send_signal(signal.SIGINT)
            try: peer.wait(timeout=5)
            except subprocess.TimeoutExpired: peer.kill(); peer.wait()
            peer_log.close()
# QEMU filter-dump emits little-endian Ethernet PCAP. No inner tunnel IP
# may appear on the NIC, including return packets from the TCP service.
capture = (root / "network.pcap").read_bytes()
if capture[:4] != b"\xd4\xc3\xb2\xa1": raise SystemExit("unexpected capture format")
at = 24
while at < len(capture):
    length = struct.unpack_from("<I", capture, at + 8)[0]
    frame = capture[at+16:at+16+length]; at += 16 + length
    if len(frame) >= 34 and frame[12:14] == b"\x08\x00":
        addresses = (b"\x0a\x4d\x00\x01", b"\x0a\x4d\x00\x02", b"\x0a\x03\xff\xfd", b"\x0a\x04\x06\x44")
        if frame[26:30] in addresses or frame[30:34] in addresses:
            raise SystemExit("plaintext tunnel packet reached the physical NIC")
print("PASS: packet capture contains no plaintext tunnel traffic")
print(f"Artifacts: {root}")
