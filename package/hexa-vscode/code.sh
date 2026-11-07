#!/bin/sh
# VS Code on native Wayland; secrets stored without a keyring (no gnome-keyring)
exec /opt/vscode/bin/code \
	--ozone-platform-hint=auto \
	--enable-features=WaylandWindowDecorations \
	--password-store=basic "$@"
