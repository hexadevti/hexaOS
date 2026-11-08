#!/bin/bash
# Runs hexaOS in QEMU like on the real laptop: UEFI firmware (OVMF) -> GRUB ->
# kernel, booting from an NVMe SSD larger than the image, so the first-boot
# partition growth (S02growfs) runs just like on real hardware.
#
# Usage (in WSL): bash scripts/run-qemu.sh [options]
#   --image PATH    boot a saved image (e.g. a frozen copy in hexaOS-images)
#                   instead of the latest build output
#   --keep          keep changes between runs in disk-config.img
#                   (configure first, then write it to a USB drive)
#   --serial        terminal only (no window)
#   --disk-size N   size of the emulated SSD (default 64G; sparse file)
#   --mem N         RAM for the VM (default 4G)
#
# The source image is never modified: the VM always runs on a copy.
set -euo pipefail

OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
IMG="$OUT_DIR/images/disk.img"
OVMF=/usr/share/ovmf/OVMF.fd
KEEP=0; SERIAL=0; DISK_SIZE=64G; MEM=4G

while [ $# -gt 0 ]; do
    case "$1" in
        --image)     IMG="$2"; shift ;;
        --keep)      KEEP=1 ;;
        --serial)    SERIAL=1 ;;
        --disk-size) DISK_SIZE="$2"; shift ;;
        --mem)       MEM="$2"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

[ -f "$IMG" ]  || { echo "Image not found: $IMG (run scripts/build.sh)"; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF not found: sudo apt install ovmf"; exit 1; }

if [ "$KEEP" = 1 ]; then
    # Persistent image: created once and reused. To start over, delete it.
    RUN_IMG="$OUT_DIR/images/disk-config.img"
    NEW=0; [ -f "$RUN_IMG" ] || NEW=1
    # The VM writes to RUN_IMG, so its date says nothing; compare against the
    # date of the build it was copied from (saved in a stamp file).
    if [ "$NEW" = 0 ] && [ "$(stat -c %Y "$IMG")" != "$(cat "$RUN_IMG.src" 2>/dev/null)" ]; then
        echo "WARNING: $RUN_IMG was made from an older build."
        echo "         You are running the OLD system with your saved changes."
        echo "         To use the new build: rm \"$RUN_IMG\" (this loses saved changes)."
    fi
else
    RUN_IMG="$OUT_DIR/images/disk-run.img"
    NEW=1
fi
if [ "$NEW" = 1 ]; then
    echo "Copying $IMG -> $RUN_IMG ..."
    mkdir -p "$(dirname "$RUN_IMG")"
    cp --sparse=always "$IMG" "$RUN_IMG"
    chmod u+w "$RUN_IMG"   # the source may be a read-only frozen copy
    # Make the "SSD" bigger than the image (sparse: uses no extra space up front)
    truncate -s "$DISK_SIZE" "$RUN_IMG"
    stat -c %Y "$IMG" > "$RUN_IMG.src"
fi
echo "VM disk: $RUN_IMG"

# Without KVM: pure emulation with a modern CPU (hexaOS requires x86-64-v2).
# For speed: sudo usermod -aG kvm $USER  (then restart WSL)
# -smap: QEMU 8.x TCG emulates a user-mode iretq as a supervisor access and
# SMAP then kills Wine (kernel oops). Real hardware is not affected.
ACCEL="-accel tcg -cpu max,-smap"
if [ -w /dev/kvm ]; then
    ACCEL="-accel kvm -cpu host"
else
    echo "Note: no access to /dev/kvm, using slow software emulation."
fi

DISPLAY_OPTS="-vga std -display gtk,zoom-to-fit=on"
[ "$SERIAL" = 1 ] && DISPLAY_OPTS="-nographic"

# Sound through WSLg's PulseAudio server, when available
AUDIO_OPTS=""
if [ -S /mnt/wslg/PulseServer ]; then
    AUDIO_OPTS="-audiodev pa,id=snd0,server=unix:/mnt/wslg/PulseServer -device intel-hda -device hda-duplex,audiodev=snd0"
fi

exec qemu-system-x86_64 \
    -name hexaOS \
    -M q35 $ACCEL -smp 4 -m "$MEM" \
    -bios "$OVMF" \
    -drive file="$RUN_IMG",if=none,id=ssd,format=raw \
    -device nvme,drive=ssd,serial=hexaos-ssd \
    -nic user,model=e1000e \
    -device qemu-xhci -device usb-kbd -device usb-tablet \
    $AUDIO_OPTS \
    $DISPLAY_OPTS
