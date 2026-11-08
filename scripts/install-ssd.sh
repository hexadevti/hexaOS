#!/bin/bash
# Linux part of install-ssd.ps1 (runs in WSL as the normal user, BEFORE anything
# is written to the SSD). Makes a copy of the EFI partition image whose boot
# menu points at the new root partition. Windows then writes the images itself,
# so the SSD never has to be detached from Windows (WSL itself may live on it).
# Usage: install-ssd.sh <root-PARTUUID>
# Output: <images>/efi-part-install.vfat
set -euo pipefail

ROOT_UUID="${1,,}"
IMAGES="${IMAGES:-$HOME/hexaos-build/images}"
HOST="${HOST_DIR:-$HOME/hexaos-build/host}/bin"
OUT="$IMAGES/efi-part-install.vfat"
CFG=/EFI/BOOT/grub.cfg

[[ $ROOT_UUID =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]] || {
    echo "Invalid PARTUUID: $ROOT_UUID" >&2; exit 1; }

cp "$IMAGES/efi-part.vfat" "$OUT"
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
"$HOST/mtype" -i "$OUT" "::$CFG" > "$TMP"
OLD=$(grep -o 'root=PARTUUID=[0-9a-fA-F-]*' "$TMP" | head -1 | cut -d= -f3)
[ -n "$OLD" ] || { echo "root=PARTUUID not found in grub.cfg" >&2; exit 1; }
sed -i "s/$OLD/$ROOT_UUID/g" "$TMP"
"$HOST/mcopy" -o -i "$OUT" "$TMP" "::$CFG"

# Check the result
N=$("$HOST/mtype" -i "$OUT" "::$CFG" | grep -c "root=PARTUUID=$ROOT_UUID")
[ "$N" -ge 1 ] || { echo "grub.cfg was not updated" >&2; exit 1; }
echo "Boot menu points at PARTUUID=$ROOT_UUID ($N entries)"
