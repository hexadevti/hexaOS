#!/bin/bash
# Parte Linux do install-ssd.ps1 (roda no WSL como root, com o disco anexado
# via "wsl --mount --bare"). Grava o hexaOS nas particoes ja criadas pelo Windows.
# Uso: install-ssd.sh <PARTUUID-da-EFI> <PARTUUID-da-raiz>
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
    echo "Particao com PARTUUID $1 nao encontrada no WSL" >&2
    return 1
}

ESP=$(find_part "$ESP_UUID")
ROOT=$(find_part "$ROOT_UUID")
echo "Particao EFI:  $ESP"
echo "Particao raiz: $ROOT"

# Seguranca: as particoes nao podem ter nada montado
if lsblk -rno MOUNTPOINTS "$ESP" "$ROOT" | grep -q .; then
    echo "Particao montada, abortando" >&2; exit 1
fi

echo "== Gravando particao EFI (grub + kernel)"
dd if="$IMAGES/efi-part.vfat" of="$ESP" bs=4M conv=fsync status=none

echo "== Gravando sistema (rootfs, ~3 GB)"
dd if="$IMAGES/rootfs.ext2" of="$ROOT" bs=4M conv=fsync status=progress

echo "== Aumentando o sistema de arquivos para ocupar a particao"
e2fsck -fy "$ROOT" || [ $? -le 1 ]
resize2fs "$ROOT"

echo "== Apontando o boot para a nova particao"
MNT=$(mktemp -d)
mount "$ESP" "$MNT"
CFG="$MNT/EFI/BOOT/grub.cfg"
OLD=$(grep -o 'root=PARTUUID=[0-9a-fA-F-]*' "$CFG" | head -1 | cut -d= -f3)
sed -i "s/$OLD/$ROOT_UUID/g" "$CFG"
grep -m1 'root=PARTUUID' "$CFG"
umount "$MNT"; rmdir "$MNT"
sync
echo "== Pronto"
