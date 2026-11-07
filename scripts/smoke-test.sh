#!/bin/bash
# Teste automatico: liga o hexaOS no QEMU (UEFI, sem janela), espera a area de
# trabalho, abre o terminal, e depois confere o sistema pelo login em texto (tty2).
# Salva capturas de tela em PNG.
# Uso (no WSL): bash scripts/smoke-test.sh [pasta-de-saida]
set -euo pipefail

OUT_DIR="${OUT_DIR:-$HOME/hexaos-build}"
SHOTS="${1:-$OUT_DIR/screenshots}"
IMG="$OUT_DIR/images/disk.img"
RUN_IMG="$OUT_DIR/images/disk-test.img"
DESKTOP_WAIT="${DESKTOP_WAIT:-60}"
mkdir -p "$SHOTS"
rm -f "$SHOTS"/*.png
cp "$IMG" "$RUN_IMG"

ACCEL="-accel tcg -cpu max"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

# Digita um texto via monitor do QEMU (sendkey)
type_text() {
    local s="$1" c k
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            " ") k=spc ;; "-") k=minus ;; "." ) k=dot ;; "/") k=slash ;;
            "|") k=shift-backslash ;; "_") k=shift-minus ;; "=") k=equal ;; ";") k=semicolon ;;
            "*") k=shift-8 ;; ">") k=shift-dot ;; "'") k=apostrophe ;; ",") k=comma ;;
            [A-Z]) k="shift-${c,,}" ;;
            *) k="$c" ;;
        esac
        echo "sendkey $k"
    done
    echo "sendkey ret"
}

{
    sleep 8
    echo "screendump $SHOTS/1-boot.png -f png"
    sleep "$DESKTOP_WAIT"
    echo "screendump $SHOTS/2-desktop.png -f png"
    # Clica no primeiro icone do painel (terminal)
    echo "mouse_move 20 14"
    sleep 1
    echo "mouse_button 1"; sleep 0.3; echo "mouse_button 0"
    sleep 15
    type_text "uname -r; free -m | head -2"
    sleep 3
    echo "screendump $SHOTS/3-terminal.png -f png"
    # Vai para o login em texto (tty2) e confere drivers e logs
    echo "sendkey ctrl-alt-f2"
    sleep 4
    type_text "root"
    sleep 3
    type_text "clear; cat /etc/os-release | head -2; date; ps | grep -E 'weston|seatd|iwd' | grep -v grep; tail -4 /tmp/weston.log; df -h /"
    sleep 4
    echo "screendump $SHOTS/4-tty2.png -f png"
    sleep 1
    echo "quit"
} | timeout $((DESKTOP_WAIT + 90)) qemu-system-x86_64 \
    -M q35 $ACCEL -smp 4 -m 2G \
    -bios /usr/share/ovmf/OVMF.fd \
    -drive file="$RUN_IMG",if=virtio,format=raw \
    -nic user,model=virtio-net-pci \
    -device qemu-xhci -device usb-tablet \
    -vga std -display none \
    -serial file:"$SHOTS/serial.log" \
    -monitor stdio > "$SHOTS/monitor.log" || true

rm -f "$RUN_IMG"
echo "Capturas em: $SHOTS"
ls -la "$SHOTS"
