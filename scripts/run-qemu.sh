#!/bin/bash
# Roda o hexaOS no QEMU com firmware UEFI (OVMF), igual ao notebook real.
# Uso (no WSL): bash scripts/run-qemu.sh            -> abre janela
#               bash scripts/run-qemu.sh --serial   -> so terminal (sem janela)
#               bash scripts/run-qemu.sh --keep     -> guarda as mudancas em disk-config.img
#                                                      (para configurar e depois gravar no pendrive)
set -euo pipefail

OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
IMG="$OUT_DIR/images/disk.img"
OVMF=/usr/share/ovmf/OVMF.fd

[ -f "$IMG" ]  || { echo "Imagem nao encontrada: $IMG (rode scripts/build.sh)"; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF nao encontrado: sudo apt install ovmf"; exit 1; }

KEEP=0; SERIAL=0
for arg in "$@"; do
    case "$arg" in
        --keep)   KEEP=1 ;;
        --serial) SERIAL=1 ;;
    esac
done

if [ "$KEEP" = 1 ]; then
    # Imagem persistente: criada uma vez a partir do disk.img e reaproveitada.
    # Para recomecar do zero, apague disk-config.img.
    RUN_IMG="$OUT_DIR/images/disk-config.img"
    [ -f "$RUN_IMG" ] || cp "$IMG" "$RUN_IMG"
    echo "Mudancas serao salvas em: $RUN_IMG"
else
    # Copia a imagem para nao alterar a original durante os testes
    RUN_IMG="$OUT_DIR/images/disk-run.img"
    cp "$IMG" "$RUN_IMG"
fi

# Sem KVM: emulacao pura com CPU moderna (o hexaOS exige x86-64-v2).
# Para ficar rapido: sudo usermod -aG kvm $USER  (e reabrir o WSL)
# -smap: o TCG do QEMU 8.x emula o iretq em modo usuario como acesso de
# supervisor e o SMAP derruba o Wine (kernel oops). Hardware real nao tem isso.
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
