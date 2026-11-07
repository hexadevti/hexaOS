#!/bin/bash
# Roda o hexaOS no QEMU com firmware UEFI (OVMF), igual ao notebook real.
# Uso (no WSL): bash scripts/run-qemu.sh            -> abre janela
#               bash scripts/run-qemu.sh --serial   -> so terminal (sem janela)
set -euo pipefail

OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
IMG="$OUT_DIR/images/disk.img"
OVMF=/usr/share/ovmf/OVMF.fd

[ -f "$IMG" ]  || { echo "Imagem nao encontrada: $IMG (rode scripts/build.sh)"; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF nao encontrado: sudo apt install ovmf"; exit 1; }

# Copia a imagem para nao alterar a original durante os testes
RUN_IMG="$OUT_DIR/images/disk-run.img"
cp "$IMG" "$RUN_IMG"

# Sem KVM: emulacao pura com CPU moderna (o hexaOS exige x86-64-v2).
# Para ficar rapido: sudo usermod -aG kvm $USER  (e reabrir o WSL)
ACCEL="-accel tcg -cpu max"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

DISPLAY_OPTS="-vga std"
[ "${1:-}" = "--serial" ] && DISPLAY_OPTS="-nographic"

exec qemu-system-x86_64 \
    -M q35 $ACCEL -smp 4 -m 2G \
    -bios "$OVMF" \
    -drive file="$RUN_IMG",if=virtio,format=raw \
    -nic user,model=virtio-net-pci -device qemu-xhci -device usb-tablet \
    $DISPLAY_OPTS
