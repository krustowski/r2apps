#!/bin/bash
# run-qemu.sh --- libjsr2's tests on r2: js.elf and a bundle of the test
# files on a scratch floppy, run from INIT.RC on the text kernel, results
# read back from the floppy (RESULT.TXT).  Prints them; exits non-zero
# unless the last line says "0 failed".
#
#   R2_MAIN     the kernel checkout (default: ../../../../r2_main)
#   R2_WORK     the work directory (default: a new temporary one)
#   R2_TIMEOUT  seconds to wait (default: 120)
#
# QEMU runs as "r2jsr2t" with its own monitor port, so killing it never
# touches another session's VM.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
K=${R2_MAIN:-$HERE/../../../../r2_main}
W=${R2_WORK:-$(mktemp -d)}
TIMEOUT=${R2_TIMEOUT:-120}
NAME=r2jsr2t
PORT=45481

pkill -f "^$NAME" 2>/dev/null || true
mkdir -p "$W/iso/boot/grub"
cp "$K/iso/boot/kernel_text.elf" "$K/iso/boot/usb.tar" "$W/iso/boot/"
cat > "$W/iso/boot/grub/grub.cfg" <<'CFG'
set timeout=0
set default=0
menuentry "r2 text" {
    multiboot2 /boot/kernel_text.elf
    module2 /boot/usb.tar usb
    boot
}
CFG
grub2-mkrescue -o "$W/r2.iso" "$W/iso" >"$W/mkrescue.log" 2>&1

cat "$HERE/harness.js" "$HERE/r2adapter.js" \
    "$HERE/language.test.js" "$HERE/timers.test.js" "$HERE/events.test.js" \
    "$HERE/url.test.js" "$HERE/encoding.test.js" "$HERE/limits.test.js" > "$W/TESTS.JS"
echo '__run();' >> "$W/TESTS.JS"
strip -o "$W/JS.ELF" "$HERE/../js.elf"
printf 'cd /mnt/fat\nfg js -o RESULT.TXT -t 500 TESTS.JS\n' > "$W/INIT.RC"
cp "$K/fat.img" "$W/fat.img"
for f in JS.ELF TESTS.JS INIT.RC; do mcopy -o -i "$W/fat.img" "$W/$f" "::$f"; done

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-accel kvm)
( exec -a "$NAME" qemu-system-x86_64 "${ACCEL[@]}" -boot d -m 1G \
    -cdrom "$W/r2.iso" -fda "$W/fat.img" -display none \
    -monitor tcp:127.0.0.1:$PORT,server,nowait >"$W/qemu.log" 2>&1 ) &

done=0
for _ in $(seq 1 "$TIMEOUT"); do
    sleep 1
    if mtype -i "$W/fat.img" ::RESULT.TXT >"$W/RESULT.TXT" 2>/dev/null && grep -q "passed, .* failed" "$W/RESULT.TXT"; then
        done=1
        break
    fi
done
echo screendump "$W/screen.ppm" | socat - TCP:127.0.0.1:$PORT >/dev/null 2>&1 || true
sleep 1
pkill -f "^$NAME" || true
cat "$W/RESULT.TXT" 2>/dev/null || true
if [ $done = 0 ]; then echo "no result after $TIMEOUT s (see $W/screen.ppm, $W/qemu.log)"; exit 1; fi
grep -q " 0 failed" "$W/RESULT.TXT"
