#!/bin/sh
# Roda depois de montar o sistema de arquivos, antes de gerar a imagem.
set -e

BOARD_DIR=$(dirname "$0")

cp -f "$BOARD_DIR/grub-efi.cfg" "$BINARIES_DIR/efi-part/EFI/BOOT/grub.cfg"

# Scripts do overlay precisam ser executaveis (o Windows nao guarda essa info)
chmod 0755 "$TARGET_DIR/usr/sbin/hexa-session" "$TARGET_DIR/etc/init.d/S00psplash"

# tty1 = sessao grafica (ou login em texto no modo "hexaos.text")
INITTAB="$TARGET_DIR/etc/inittab"
grep -q hexa-session "$INITTAB" || \
    sed -i '/GENERIC_SERIAL/i tty1::respawn:/usr/sbin/hexa-session' "$INITTAB"
