#!/bin/bash
# run-qemu.sh <result>[,<result>...] <file>[:<NAME>]... --- boot r2's text
# kernel with the files on a scratch floppy, wait for the programs to write
# their results to the floppy, and print them.
#
# Each file goes to the floppy's root as NAME (8.3), or as its basename in
# capitals; one of them should be INIT.RC.  Programs run from the floppy's
# working directory, since fg looks there before the bin paths.  The last
# result named is taken as complete once it has stopped growing.
#
#   R2_MAIN     the kernel checkout (default: ../../../r2_main)
#   R2_ISO      an image to boot as it is, in place of a text-kernel one built
#               from R2_MAIN's kernel and boot archive
#   R2_TAR_ADD  a directory whose contents are added to the boot archive
#   R2_WORK     the work directory (default: a new temporary one)
#   R2_TIMEOUT  seconds to wait for <result> (default: 120)
#
# QEMU runs as "r2tcct", so a kill never touches another session's VM.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
RESULTS=${1//,/ }
RESULT=${RESULTS##* }
shift
K=${R2_MAIN:-$HERE/../../../r2_main}
W=${R2_WORK:-$(mktemp -d)}
TIMEOUT=${R2_TIMEOUT:-120}
NAME=r2tcct
PORT=45471

pkill -f "^$NAME" 2>/dev/null || true
rm -rf "$W"
mkdir -p "$W/iso/boot/grub"
if [ -n "${R2_ISO:-}" ]; then
    ISO=$(realpath "$R2_ISO")
else
    ISO=$W/r2.iso
    cp "$K/iso/boot/kernel_text.elf" "$W/iso/boot/"
    if [ -n "${R2_TAR_ADD:-}" ]; then
        mkdir -p "$W/tar"
        tar -xf "$K/iso/boot/usb.tar" -C "$W/tar"
        cp -r "$R2_TAR_ADD"/. "$W/tar/"
        tar --format=ustar -cf "$W/iso/boot/usb.tar" -C "$W/tar" .
    else
        cp "$K/iso/boot/usb.tar" "$W/iso/boot/"
    fi
    cat > "$W/iso/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0
menuentry "r2 text" {
    multiboot2 /boot/kernel_text.elf
    module2 /boot/usb.tar usb
    boot
}
EOF
    grub2-mkrescue -o "$ISO" "$W/iso" >"$W/mkrescue.log" 2>&1
fi

cp "$K/fat.img" "$W/fat.img"
for spec in "$@"; do
    src=${spec%%:*}
    dst=${spec#*:}
    [ "$dst" = "$spec" ] && dst=$(basename "$src" | tr '[:lower:]' '[:upper:]')
    mcopy -o -i "$W/fat.img" "$src" "::$dst"
done

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-accel kvm)
( exec -a "$NAME" qemu-system-x86_64 "${ACCEL[@]}" -boot d -m 2G \
    -cdrom "$ISO" -fda "$W/fat.img" -display none \
    -monitor tcp:127.0.0.1:$PORT,server,nowait >"$W/qemu.log" 2>&1 ) &

last=-1
stable=0
for _ in $(seq 1 "$TIMEOUT"); do
    sleep 1
    if mtype -i "$W/fat.img" "::$RESULT" >"$W/$RESULT" 2>/dev/null && [ -s "$W/$RESULT" ]; then
        size=$(stat -c %s "$W/$RESULT")
        if [ "$size" = "$last" ]; then
            stable=$((stable + 1))
            [ $stable -ge 2 ] && break
        else
            stable=0
            last=$size
        fi
    fi
done
echo screendump "$W/screen.ppm" | socat - TCP:127.0.0.1:$PORT >/dev/null 2>&1 || true
sleep 1
pkill -f "^$NAME" || true

status=0
for r in $RESULTS; do
    if mtype -i "$W/fat.img" "::$r" >"$W/$r" 2>/dev/null && [ -s "$W/$r" ]; then
        cat "$W/$r"
    else
        echo "no $r after $TIMEOUT s (see $W/screen.ppm, $W/qemu.log)"
        status=1
    fi
done
exit $status
