#!/bin/bash
# Builds hexaOS. Run inside WSL:  bash scripts/build.sh [make-target]
# E.g. bash scripts/build.sh            -> builds everything
#      bash scripts/build.sh menuconfig -> opens the configuration menu
set -euo pipefail

HEXA_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BR_DIR="${BR_DIR:-$HOME/buildroot}"
OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
BR_VERSION="2026.02.3"

# Buildroot rejects a PATH with spaces (WSL inherits the Windows PATH)
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

if [ ! -d "$BR_DIR" ]; then
    git clone --depth 1 --branch "$BR_VERSION" https://gitlab.com/buildroot.org/buildroot.git "$BR_DIR"
fi

# (Re)apply the hexaOS configuration the first time or when the defconfig changes
DEFCONFIG="$HEXA_DIR/configs/hexaos_x86_64_defconfig"
if [ ! -f "$OUT_DIR/.config" ] || [ "$DEFCONFIG" -nt "$OUT_DIR/.config" ]; then
    make -C "$BR_DIR" O="$OUT_DIR" BR2_EXTERNAL="$HEXA_DIR" hexaos_x86_64_defconfig
fi

make -C "$BR_DIR" O="$OUT_DIR" "$@"

if [ $# -eq 0 ]; then
    echo
    echo "Done: $OUT_DIR/images/disk.img"
fi
