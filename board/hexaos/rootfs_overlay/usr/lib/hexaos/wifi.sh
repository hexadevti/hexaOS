# Wi-Fi helpers shared by hexa-settings and hexa-setup (bash; source this file).
# iwd does the work; these functions wrap "iwctl" and explain failures.

# Removes terminal control sequences (colors, cursor moves) from iwctl output.
# BusyBox sed does not understand "\x1b", so bash puts a real ESC in the expression.
hx_strip_colors() { sed $'s/\e\\[[0-9;?]*[A-Za-z]//g; s/\r$//'; }

# Name of the first Wi-Fi interface (wlan0...), empty if there is none
hx_wifi_iface() {
    local dev
    for dev in /sys/class/net/*; do
        [ -d "$dev/wireless" ] && { echo "${dev##*/}"; return; }
    done
}

# hx_wifi_state IFACE -> connected, disconnected, connecting...
hx_wifi_state() {
    iwctl station "$1" show 2>/dev/null | hx_strip_colors | awk '$1 == "State" {print $2}'
}

# hx_wifi_wait IFACE SECONDS: succeeds as soon as the interface is connected
hx_wifi_wait() {
    local i
    for ((i = 0; i < $2; i++)); do
        [ "$(hx_wifi_state "$1")" = connected ] && return 0
        sleep 1
    done
    return 1
}

# Line number the kernel log has reached, so hx_wifi_diagnose only looks at what came after
hx_wifi_mark() { dmesg 2>/dev/null | wc -l; }

# hx_wifi_diagnose IFACE IWCTL_OUTPUT [DMESG_MARK]
# Prints a short explanation of why the last connection attempt failed, telling
# a rejected password apart from a radio/driver problem, followed by the raw
# kernel and iwd messages.
hx_wifi_diagnose() {
    local W=$1 out=$2 mark=${3:-0} k iwdlog rf verdict
    k=$(dmesg 2>/dev/null | tail -n +"$((mark + 1))" | grep -E "(^|[] ])$W: |iwlwifi" |
        sed 's/^\[[^]]*\] *//' | tail -n 8 | cut -c1-74)
    iwdlog=$(grep -h ' iwd: ' /var/log/messages 2>/dev/null |
             grep -iE 'fail|error|reject|handshake|password|passphrase|sae|psk|status|reason|timeout|timed out' |
             tail -n 6 | sed 's/^.* iwd: //' | cut -c1-74)

    for rf in /sys/class/rfkill/rfkill*; do
        [ "$(cat "$rf/type" 2>/dev/null)" = wlan ] || continue
        [ "$(cat "$rf/hard" 2>/dev/null)" = 1 ] && verdict="Wi-Fi is switched off by a hardware switch or the BIOS (rfkill hard block)."
        [ "$(cat "$rf/soft" 2>/dev/null)" = 1 ] && verdict="Wi-Fi is turned off (rfkill soft block). Turn it on in Settings > Wi-Fi."
    done

    if [ -z "$verdict" ]; then
        case "$out$k$iwdlog" in
            *"Invalid format"*|*"Invalid passphrase"*|*"nvalid argument"*)
                verdict="The password was not accepted: a WPA password has 8 to 63 characters." ;;
            *"Reason: 15"*|*4WAY_HANDSHAKE*|*"Reason: 2="*|*"Reason: 23"*|*"handshake failed"*|*"SAE"*"fail"*|*"status: 15"*|*"status 15"*)
                verdict="The router REJECTED THE PASSWORD (the Wi-Fi card works).\nCheck it letter by letter, including upper/lower case." ;;
            *"Not found"*|*"not found"*)
                verdict="The network was not found. Move closer to the router or check its name." ;;
            *"timed out"*|*"Reason: 4="*)
                verdict="The router did not answer. The signal may be weak, or the Wi-Fi\ncard/driver has a problem (the password was not tested)." ;;
            *"No Agent"*|*"Aborted"*|*"Canceled"*)
                verdict="No password was given to iwd." ;;
            *"Operation already in progress"*|*"busy"*)
                verdict="Another connection attempt was still running. Try again in a few seconds." ;;
            *)
                if [ -z "$k" ]; then
                    verdict="The Wi-Fi card never tried to reach the router (no kernel messages)."
                else
                    verdict="The reason is not clear: see the messages below."
                fi ;;
        esac
    fi

    printf '%b\n' "$verdict"
    [ -n "$out" ] && printf '\niwctl said: %s\n' "$(echo "$out" | hx_strip_colors | tr -s '\n' ' ' | cut -c1-70)"
    [ -n "$k" ] && printf '\nKernel messages:\n%s\n' "$k"
    [ -n "$iwdlog" ] && printf '\niwd messages:\n%s\n' "$iwdlog"
    return 0
}
