################################################################################
#
# hexa-vscode: official Visual Studio Code binary
#
################################################################################

HEXA_VSCODE_VERSION = 1793945155
HEXA_VSCODE_COMMIT = c4453f8ea430795f941c1b2ff6c9321135a74a02
HEXA_VSCODE_SOURCE = code-stable-x64-$(HEXA_VSCODE_VERSION).tar.gz
HEXA_VSCODE_SITE = https://vscode.download.prss.microsoft.com/dbazure/download/stable/$(HEXA_VSCODE_COMMIT)
HEXA_VSCODE_LICENSE = Microsoft Software License (proprietary)
HEXA_VSCODE_DEPENDENCIES = libgtk3 at-spi2-core libnss cups dbus expat \
	alsa-lib libxkbcommon mesa3d libxcb xlib_libX11 xlib_libXcomposite \
	xlib_libXdamage xlib_libXext xlib_libXfixes xlib_libXrandr xlib_libxkbfile

define HEXA_VSCODE_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/opt/vscode
	mkdir -p $(TARGET_DIR)/opt
	cp -a $(@D) $(TARGET_DIR)/opt/vscode
	rm -rf $(TARGET_DIR)/opt/vscode/.[!.]*
	# Microsoft account login broker (needs WebKitGTK); everything else works without it
	rm -f $(TARGET_DIR)/opt/vscode/resources/app/extensions/microsoft-authentication/dist/libmsalruntime.so
	$(INSTALL) -D -m 0755 $(HEXA_VSCODE_PKGDIR)/code.sh $(TARGET_DIR)/usr/bin/code
endef

$(eval $(generic-package))
