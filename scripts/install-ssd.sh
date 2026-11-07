#!/bin/bash
# Linux part of install-ssd.ps1 (runs in WSL as root, with the disk attached
# via "wsl --mount --bare"). Writes hexaOS to the partitions created by Windows.
# Usage: install-ssd.sh <EFI-PARTUUID> <root-PARTUUID>
set -euo pipefail

ESP_UUID="${1,,}"
ROOT_UUID="${2,,}"
IMAGES="${IMAGES:-/home/luciano/hexaos-build/images}"

find_part() {
    local dev
    for _ in $(seq 1 20); do
        dev=$(lsblk -rno PATH,PARTUUID | awk -v u="$1" 'tolower($2) == u { print $1 }')
        [ -n "$dev" ] && { echo "$dev"; return 0; }
        sleep 1
    done
    echo "Partition with PARTUUID $1 not found in WSL" >&2
    return 1
}

ESP=$(find_part "$ESP_UUID")
ROOT=$(find_part "$ROOT_UUID")
echo "EFI partition:  $ESP"
echo "Root partition: $ROOT"

# Safety: nothing may be mounted from these partitions
if lsblk -rno MOUNTPOINTS "$ESP" "$ROOT" | grep -q .; then
    echo "Partition is mounted, aborting" >&2; exit 1
fi

echo "== Writing EFI partition (grub + kernel)"
dd if="$IMAGES/efi-part.vfat" of="$ESP" bs=4M conv=fsync status=none

echo "== Writing system (rootfs)"
dd if="$IMAGES/rootfs.ext2" of="$ROOT" bs=4M conv=fsync status=progress

echo "== Growing the filesystem to fill the partition"
e2fsck -fy "$ROOT" || [ $? -le 1 ]
resize2fs "$ROOT"

echo "== Pointing the boot config at the new partition"
MNT=$(mktemp -d)
mount "$ESP" "$MNT"
CFG="$MNT/EFI/BOOT/grub.cfg"
OLD=$(grep -o 'root=PARTUUID=[0-9a-fA-F-]*' "$CFG" | head -1 | cut -d= -f3)
sed -i "s/$OLD/$ROOT_UUID/g" "$CFG"
grep -m1 'root=PARTUUID' "$CFG"
umount "$MNT"; rmdir "$MNT"
sync
echo "== Done"
