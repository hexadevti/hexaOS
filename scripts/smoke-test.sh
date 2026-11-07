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
DESKTOP_WAIT="${DESKTOP_WAIT:-45}"
QMP_SOCK="/tmp/hexaos-qmp-$$.sock"
mkdir -p "$SHOTS"
rm -f "$SHOTS"/*.png
cp "$IMG" "$RUN_IMG"

# -smap: o TCG do QEMU 8.x emula o iretq em modo usuario como acesso de
# supervisor e o SMAP derruba o Wine (kernel oops). Hardware real nao tem isso.
ACCEL="-accel tcg -cpu max,-smap"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

# Digita um texto via monitor do QEMU (sendkey)
type_text() {
    local s="$1" c k
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            " ") k=spc ;; "-") k=minus ;; "." ) k=dot ;; "/") k=slash ;;
            "|") k=shift-backslash ;; "_") k=shift-minus ;; "=") k=equal ;; ";") k=semicolon ;;
            "*") k=shift-8 ;; ">") k=shift-dot ;; "'") k=apostrophe ;; ",") k=comma ;; "&") k=shift-7 ;;
            [A-Z]) k="shift-${c,,}" ;;
            *) k="$c" ;;
        esac
        echo "sendkey $k"
    done
    echo "sendkey ret"
}

{
    # Algumas capturas durante o boot (tela de splash)
    for t in 6 10 14 18; do
        sleep 4
        echo "screendump $SHOTS/1-boot-${t}s.png -f png"
    done
    sleep "$DESKTOP_WAIT"
    echo "screendump $SHOTS/2-desktop.png -f png"
    # Clica no primeiro icone do painel (terminal)
    python3 "$(dirname "$0")/qmp-click.py" "$QMP_SOCK" 20 15 1280 800 >&2 || true
    sleep 15
    # Teclado do hexaOS e ABNT2: usar so letras, numeros, espaco, - . /
    type_text "uname -r"
    type_text "free -m"
    type_text "wine --version"
    sleep 3
    echo "screendump $SHOTS/3-terminal.png -f png"
    # Abre o Bloco de Notas do Wine (terceiro icone do painel)
    if [ "${WINE_TEST:-1}" = 1 ]; then
        python3 "$(dirname "$0")/qmp-click.py" "$QMP_SOCK" 80 15 1280 800 >&2 || true
        sleep "${WINE_WAIT:-60}"
        echo "screendump $SHOTS/3b-wine.png -f png"
    fi
    # Vai para o login em texto (tty2) e confere drivers e logs
    echo "sendkey ctrl-alt-f2"
    sleep 4
    type_text "root"
    sleep 3
    type_text "clear; cat /etc/os-release | head -2; date; ps | grep -E 'weston|seatd|iwd|wine|notepad' | grep -v grep | cut -c1-90; tail -4 /tmp/weston.log; df -h /; free -m"
    sleep 4
    echo "screendump $SHOTS/4-tty2.png -f png"
    sleep 1
    echo "quit"
} | timeout $((DESKTOP_WAIT + ${WINE_WAIT:-60} + 120)) qemu-system-x86_64 \
    -M q35 $ACCEL -smp 4 -m 2G \
    -bios /usr/share/ovmf/OVMF.fd \
    -drive file="$RUN_IMG",if=virtio,format=raw \
    -nic user,model=virtio-net-pci \
    -device qemu-xhci -device usb-tablet \
    -vga std -display none \
    -serial file:"$SHOTS/serial.log" \
    -qmp unix:"$QMP_SOCK",server=on,wait=off \
    -monitor stdio > "$SHOTS/monitor.log" || true

rm -f "$RUN_IMG" "$QMP_SOCK"
echo "Capturas em: $SHOTS"
ls -la "$SHOTS"
