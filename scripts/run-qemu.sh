#!/bin/bash
# Runs hexaOS in QEMU with UEFI firmware (OVMF), like the real laptop.
# Usage (in WSL): bash scripts/run-qemu.sh            -> opens a window
#                 bash scripts/run-qemu.sh --serial   -> terminal only (no window)
#                 bash scripts/run-qemu.sh --keep     -> keeps changes in disk-config.img
#                                                        (configure first, then write it to a USB drive)
set -euo pipefail

OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
IMG="$OUT_DIR/images/disk.img"
OVMF=/usr/share/ovmf/OVMF.fd

[ -f "$IMG" ]  || { echo "Image not found: $IMG (run scripts/build.sh)"; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF not found: sudo apt install ovmf"; exit 1; }

KEEP=0; SERIAL=0
for arg in "$@"; do
    case "$arg" in
        --keep)   KEEP=1 ;;
        --serial) SERIAL=1 ;;
    esac
done

if [ "$KEEP" = 1 ]; then
    # Persistent image: created once from disk.img and reused.
    # To start over, delete disk-config.img.
    RUN_IMG="$OUT_DIR/images/disk-config.img"
    [ -f "$RUN_IMG" ] || cp "$IMG" "$RUN_IMG"
    echo "Changes will be saved to: $RUN_IMG"
else
    # Copy the image so tests do not modify the original
    RUN_IMG="$OUT_DIR/images/disk-run.img"
    cp "$IMG" "$RUN_IMG"
fi

# Without KVM: pure emulation with a modern CPU (hexaOS requires x86-64-v2).
# For speed: sudo usermod -aG kvm $USER  (then restart WSL)
# -smap: QEMU 8.x TCG emulates a user-mode iretq as a supervisor access and
# SMAP then kills Wine (kernel oops). Real hardware is not affected.
ACCEL="-accel tcg -cpu max,-smap"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

DISPLAY_OPTS="-vga std"
[ "$SERIAL" = 1 ] && DISPLAY_OPTS="-nographic"

exec qemu-system-x86_64 \
    -M q35 $ACCEL -smp 4 -m 2G \
    -bios "$OVMF" \
    -drive file="$RUN_IMG",if=virtio,format=raw \
    -nic user,model=virtio-net-pci -device qemu-xhci -device usb-tablet \
    $DISPLAY_OPTS
