#!/bin/bash
# Automated test: boots hexaOS in QEMU (UEFI, headless), waits for the desktop,
# opens the terminal, then checks the system through the text login (tty2).
# Saves PNG screenshots.
# Usage (in WSL): bash scripts/smoke-test.sh [output-dir]
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

# -smap: QEMU 8.x TCG emulates a user-mode iretq as a supervisor access and
# SMAP then kills Wine (kernel oops). Real hardware is not affected.
ACCEL="-accel tcg -cpu max,-smap"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

# Types text through the QEMU monitor (sendkey)
type_text() {
    local s="$1" c k
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            # Key positions of the ABNT2 layout used by hexaOS (QEMU sends US scancodes)
            " ") k=spc ;; "-") k=minus ;; "." ) k=dot ;; "/") k=ro ;;
            "|") k=shift-less ;; "_") k=shift-minus ;; "=") k=equal ;; ";") k=slash ;;
            "*") k=shift-8 ;; ">") k=shift-dot ;; "'") k=grave_accent ;; ",") k=comma ;; "&") k=shift-7 ;;
            [A-Z]) k="shift-${c,,}" ;;
            *) k="$c" ;;
        esac
        echo "sendkey $k"
    done
    echo "sendkey ret"
}

{
    # A few screenshots during boot (splash screen)
    for t in 6 10 14 18; do
        sleep 4
        echo "screendump $SHOTS/1-boot-${t}s.png -f png"
    done
    sleep "$DESKTOP_WAIT"
    echo "screendump $SHOTS/2-desktop.png -f png"
    # Click the first panel icon (terminal)
    python3 "$(dirname "$0")/qmp-click.py" "$QMP_SOCK" 20 15 1280 800 >&2 || true
    sleep 15
    # type_text maps characters to ABNT2 key positions
    type_text "uname -r"
    type_text "free -m"
    type_text "wine --version"
    sleep 3
    echo "screendump $SHOTS/3-terminal.png -f png"
    # Open Wine Notepad (third panel icon)
    if [ "${WINE_TEST:-1}" = 1 ]; then
        python3 "$(dirname "$0")/qmp-click.py" "$QMP_SOCK" 80 15 1280 800 >&2 || true
        sleep "${WINE_WAIT:-60}"
        echo "screendump $SHOTS/3b-wine.png -f png"
    fi
    # Open Firefox (fourth panel icon) and VS Code (fifth)
    if [ "${APPS_TEST:-1}" = 1 ]; then
        python3 "$(dirname "$0")/qmp-click.py" "$QMP_SOCK" "${FIREFOX_X:-110}" 15 1280 800 >&2 || true
        sleep "${APP_WAIT:-60}"
        echo "screendump $SHOTS/3c-firefox.png -f png"
        python3 "$(dirname "$0")/qmp-click.py" "$QMP_SOCK" "${VSCODE_X:-140}" 15 1280 800 >&2 || true
        sleep "${APP_WAIT:-60}"
        echo "screendump $SHOTS/3d-vscode.png -f png"
    fi
    # Switch to the text login (tty2) and check drivers and logs
    echo "sendkey ctrl-alt-f2"
    sleep 4
    type_text "root"
    sleep 3
    type_text "clear; cat /etc/os-release | head -2; date; ps | grep -E 'weston|seatd|iwd|wine|notepad' | grep -v grep | cut -c1-90; df -h /; free -m"
    sleep 4
    echo "screendump $SHOTS/4a-tty2.png -f png"
    type_text "clear; tail -40 /tmp/weston.log"
    sleep 4
    echo "screendump $SHOTS/4-tty2.png -f png"
    type_text "clear; python3 --version; git --version; docker --version; ls /etc/init.d; ps | grep -E 'dockerd|containerd|firefox|code' | grep -v grep | cut -c1-90"
    sleep 6
    echo "screendump $SHOTS/5-dev.png -f png"
    sleep 1
    echo "quit"
} | timeout $((DESKTOP_WAIT + ${WINE_WAIT:-60} + 2 * ${APP_WAIT:-60} + 150)) qemu-system-x86_64 \
    -M q35 $ACCEL -smp 4 -m 3G \
    -bios /usr/share/ovmf/OVMF.fd \
    -drive file="$RUN_IMG",if=virtio,format=raw \
    -nic user,model=virtio-net-pci \
    -device qemu-xhci -device usb-tablet \
    -vga std -display none \
    -serial file:"$SHOTS/serial.log" \
    -qmp unix:"$QMP_SOCK",server=on,wait=off \
    -monitor stdio > "$SHOTS/monitor.log" || true

rm -f "$RUN_IMG" "$QMP_SOCK"
echo "Screenshots in: $SHOTS"
ls -la "$SHOTS"
