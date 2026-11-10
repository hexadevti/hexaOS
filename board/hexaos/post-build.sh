#!/bin/sh
# Runs after the root filesystem is assembled, before the image is created.
set -e

BOARD_DIR=$(dirname "$0")

cp -f "$BOARD_DIR/grub-efi.cfg" "$BINARIES_DIR/efi-part/EFI/BOOT/grub.cfg"

# Overlay scripts must be executable (Windows does not keep that bit)
chmod 0755 "$TARGET_DIR/usr/sbin/hexa-session" "$TARGET_DIR/usr/bin/hexa-dev" \
    "$TARGET_DIR"/etc/init.d/S00psplash "$TARGET_DIR"/etc/init.d/S01keymap "$TARGET_DIR"/etc/init.d/S02growfs \
    "$TARGET_DIR"/etc/init.d/S41wired "$TARGET_DIR/usr/libexec/hexaos-wired" \
    "$TARGET_DIR/usr/bin/hexa-settings-tui" "$TARGET_DIR/usr/bin/hexa-setup" \
    "$TARGET_DIR/usr/bin/hexa-power" "$TARGET_DIR/usr/bin/hexa-steam" "$TARGET_DIR/usr/bin/hexa-vpn" \
    "$TARGET_DIR"/etc/init.d/S12speakers \
    "$TARGET_DIR"/etc/init.d/S40iwd
chmod 0440 "$TARGET_DIR/etc/sudoers.d/wheel" "$TARGET_DIR/etc/sudoers.d/power"

# hexa-setup values hold passwords (Wi-Fi...): root only. Optional (not in git).
[ -f "$TARGET_DIR/etc/hexaos/setup.conf" ] && chmod 0600 "$TARGET_DIR/etc/hexaos/setup.conf"

# Docker does not start at boot (saves memory and boot time): hexa-dev starts it
# when needed, and Settings > Services can turn on "start at boot" (S60dockerd link)
if [ -f "$TARGET_DIR/etc/init.d/S60dockerd" ] && [ ! -L "$TARGET_DIR/etc/init.d/S60dockerd" ]; then
    mv -f "$TARGET_DIR/etc/init.d/S60dockerd" "$TARGET_DIR/etc/init.d/dockerd"
fi

# tty1 = graphical session (or text login in "hexaos.text" mode)
INITTAB="$TARGET_DIR/etc/inittab"
grep -q hexa-session "$INITTAB" || \
    sed -i '/GENERIC_SERIAL/i tty1::respawn:/usr/sbin/hexa-session' "$INITTAB"
