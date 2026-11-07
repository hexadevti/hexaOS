#!/bin/sh
# Builds disk.img (GPT: EFI partition + ext4 root), ready for QEMU or a USB drive.
set -e

BOARD_DIR=$(dirname "$0")

UUID=$(dumpe2fs "$BINARIES_DIR/rootfs.ext2" 2>/dev/null | sed -n 's/^Filesystem UUID: *\(.*\)/\1/p')
sed -i "s/UUID_TMP/$UUID/g" "$BINARIES_DIR/efi-part/EFI/BOOT/grub.cfg"
sed "s/UUID_TMP/$UUID/g" "$BOARD_DIR/genimage-efi.cfg" > "$BINARIES_DIR/genimage-efi.cfg"
support/scripts/genimage.sh -c "$BINARIES_DIR/genimage-efi.cfg"
