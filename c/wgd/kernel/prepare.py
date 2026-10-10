#!/usr/bin/env python3
"""Prepare the companion kernel changes in a scratch tree, preserving local edits."""
from pathlib import Path
import argparse, difflib, shutil

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
source, dest = args.source.resolve(), args.destination.resolve()
if source == dest:
    parser.error("use a scratch destination; review/apply r2.patch after validation")
dest.mkdir(parents=True, exist_ok=True)
for name in ("src", ".cargo"):
    shutil.copytree(source / name, dest / name, dirs_exist_ok=True)
for name in ("Cargo.toml", "Cargo.lock", "build.rs", "linker.ld", "rust-toolchain.toml", "x86_64-r2.json", "Makefile", "terminus-font.psf"):
    shutil.copy2(source / name, dest / name)
(dest / "iso/boot").mkdir(parents=True, exist_ok=True)
shutil.copy2(source / "iso/boot/boot.asm", dest / "iso/boot/boot.asm")

changes = {}
def replace(name, old, new):
    path = dest / name
    text = path.read_text()
    if new in text:
        changes[name] = text
        return
    if text.count(old) != 1:
        raise SystemExit(f"expected one integration point in {name}; review kernel changes")
    path.write_text(text.replace(old, new))
    changes[name] = path.read_text()

replace("src/net/mod.rs", "pub mod udp;", "pub mod udp;\npub mod tunnel;")
replace("src/time/rtc.rs", "    unsafe {\n        // Tell CMOS what address is wanted", "    // The text-mode clock also uses CMOS from the timer interrupt. Keep the\n    // register selection and data read together so it cannot change the index.\n    x86_64::instructions::interrupts::without_interrupts(|| unsafe {\n        // Tell CMOS what address is wanted")
if "without_interrupts(|| unsafe {" in (dest / "src/time/rtc.rs").read_text():
    replace("src/time/rtc.rs", "        value\n    }\n}", "        value\n    })\n}")
name = "src/net/tunnel.rs"
(dest / name).write_text(Path(__file__).with_name("tunnel.rs").read_text())
changes[name] = (dest / name).read_text()
replace("src/net/netdrv.rs", "        if NET_DRV_PID == pid {", "        crate::net::tunnel::detach(pid);\n        if NET_DRV_PID == pid {")
replace("src/net/netdrv.rs", "    let dest_pid = tcp_dest_port(frame)\n        .and_then(|port| lookup_port(port))\n        .unwrap_or(NET_DRV_PID);",
        "    let dest_pid = crate::net::tunnel::udp_owner(frame)\n        .or_else(|| tcp_dest_port(frame).and_then(|port| lookup_port(port)))\n        .unwrap_or(NET_DRV_PID);")
replace("src/net/netdrv.rs", "        done[n] = pid;\n        n += 1;\n        queue_to(pid, frame);\n    }\n}",
        "        done[n] = pid;\n        n += 1;\n        queue_to(pid, frame);\n    }\n    if let Some(pid) = crate::net::tunnel::owner() {\n        if pid != NET_DRV_PID && !done[..n].contains(&pid) { queue_to(pid, frame); }\n    }\n}")
replace("src/net/netdrv.rs", "if NET_DRV_PID == NO_PID && !any_port_bound() {", "if NET_DRV_PID == NO_PID && !any_port_bound() && crate::net::tunnel::owner().is_none() {")
name = "src/net/netdrv.rs"
text = (dest / name).read_text()
old_guard = """        // Inner tunnel addresses never arrive directly from the physical NIC.
        if frame.len() >= 34 && frame[12..14] == [8, 0] &&
           crate::net::tunnel::is_inner([frame[30], frame[31], frame[32], frame[33]]) {
            nic::consume_frame(len);
            continue;
        }
"""
new_guard = """        // Physical input cannot impersonate authenticated tunnel input or queued output.
        if crate::net::tunnel::reject_nic(frame) {
            nic::consume_frame(len);
            continue;
        }
"""
if old_guard in text:
    replace(name, old_guard, new_guard)
else:
    replace(name, "        if deliver(frame) {", new_guard + "        if deliver(frame) {")

replace("src/net/loopback.rs", "ip[0] == 127 || (own != 0 && u32::from_be_bytes(ip) == own)",
        "ip[0] == 127 || (own != 0 && u32::from_be_bytes(ip) == own) ||\n        x86_64::instructions::interrupts::without_interrupts(|| unsafe { crate::net::tunnel::is_local(ip) })")
replace("src/abi/syscall.rs", "                        if !crate::net::loopback::transmit(slice, pid) {", "                        match x86_64::instructions::interrupts::without_interrupts(||\n                            crate::net::tunnel::transmit(slice, pid)) {\n                            Some(true) => return SyscallReturnCode::Ok as u64,\n                            Some(false) => return SyscallReturnCode::Busy as u64,\n                            None => {}\n                        }\n                        if !crate::net::loopback::transmit(slice, pid) {")
# Replace an older tunnel ABI as well as installing into an unmodified kernel.
name = "src/abi/syscall.rs"
text = (dest / name).read_text()
unknown = "        /*\n         *  Unknown syscall"
start = text.find("        /* Userspace IPv4 tunnel")
end = text.index(unknown, max(start, 0))
if start < 0:
    start = end
text = text[:start] + Path(__file__).with_name("syscall.inc").read_text() + text[end:]
(dest / name).write_text(text)
changes[name] = text

replace("Makefile", "\t@cp ../r2_app/c/chat/chat.elf ${BIN_PATH}/", "\t@$(MAKE) -C ../r2_app/c/wgd build\n\t@# Relink chat with libcr2's tunnel-address reply fix.\n\t@$(MAKE) -C ../r2_app/c/chat build\n\t@cp ../r2_app/c/chat/chat.elf ${BIN_PATH}/")
replace("Makefile", "\t@cp ../r2_app/c/eth/eth.elf ${BIN_PATH}/", "\t@cp ../r2_app/c/eth/eth.elf ${BIN_PATH}/\n\t@cp ../r2_app/c/wgd/wgd.elf ${BIN_PATH}/\n\t@mkdir -p ./iso/opt/wgd\n\t@cp ../r2_app/c/wgd/WGD.CFG.example ./iso/opt/wgd/wgd.cfg.example")

replace("Makefile", "\t@cp ../r2_app/c/r2sh/r2sh.elf ${BIN_PATH}/sh.elf",
        "\t@$(MAKE) -C ../r2_app/c/r2sh build\n\t@cp ../r2_app/c/r2sh/r2sh.elf ${BIN_PATH}/sh.elf")
replace("Makefile", "\t@cp ../r2_app/c/tnt/tnt.elf ${BIN_PATH}/",
        "\t@$(MAKE) -C ../r2_app/c/tnt build\n\t@cp ../r2_app/c/tnt/tnt.elf ${BIN_PATH}/")

patch = []
for name, updated in changes.items():
    original = (source / name).read_text() if (source / name).exists() else ""
    patch.extend(difflib.unified_diff(original.splitlines(True), updated.splitlines(True),
                                    fromfile="a/" + name if (source / name).exists() else "/dev/null", tofile="b/" + name))
Path(__file__).with_name("r2.patch").write_text("".join(patch))
print(f"Prepared {dest}; review kernel/r2.patch before applying to the kernel checkout.")
