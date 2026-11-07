#!/bin/bash
# Compila o hexaOS. Rodar dentro do WSL:  bash scripts/build.sh [alvo-make]
# Ex.: bash scripts/build.sh            -> compila tudo
#      bash scripts/build.sh menuconfig -> abre o menu de configuracao
set -euo pipefail

HEXA_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BR_DIR="${BR_DIR:-$HOME/buildroot}"
OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
BR_VERSION="2026.02.3"

# O Buildroot recusa PATH com espacos (o WSL herda o PATH do Windows)
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

if [ ! -d "$BR_DIR" ]; then
    git clone --depth 1 --branch "$BR_VERSION" https://gitlab.com/buildroot.org/buildroot.git "$BR_DIR"
fi

# (Re)aplica a configuracao do hexaOS na primeira vez ou quando o defconfig mudar
DEFCONFIG="$HEXA_DIR/configs/hexaos_x86_64_defconfig"
if [ ! -f "$OUT_DIR/.config" ] || [ "$DEFCONFIG" -nt "$OUT_DIR/.config" ]; then
    make -C "$BR_DIR" O="$OUT_DIR" BR2_EXTERNAL="$HEXA_DIR" hexaos_x86_64_defconfig
fi

make -C "$BR_DIR" O="$OUT_DIR" "$@"

if [ $# -eq 0 ]; then
    echo
    echo "Pronto: $OUT_DIR/images/disk.img"
fi
