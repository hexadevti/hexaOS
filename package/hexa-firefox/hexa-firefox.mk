################################################################################
#
# hexa-firefox: official Firefox ESR binary (en-US)
#
################################################################################

HEXA_FIREFOX_VERSION = 153.6.0esr
HEXA_FIREFOX_SOURCE = firefox-$(HEXA_FIREFOX_VERSION).tar.xz
HEXA_FIREFOX_SITE = https://ftp.mozilla.org/pub/firefox/releases/$(HEXA_FIREFOX_VERSION)/linux-x86_64/en-US
HEXA_FIREFOX_LICENSE = MPL-2.0
HEXA_FIREFOX_DEPENDENCIES = libgtk3 dbus alsa-lib fontconfig freetype \
	libxcb xlib_libX11 xlib_libXcomposite xlib_libXcursor xlib_libXdamage \
	xlib_libXext xlib_libXfixes xlib_libXi xlib_libXrandr xlib_libXrender

# The tarball contains a "firefox/" directory; Buildroot strips that first level.
define HEXA_FIREFOX_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/opt/firefox
	mkdir -p $(TARGET_DIR)/opt
	cp -a $(@D) $(TARGET_DIR)/opt/firefox
	rm -rf $(TARGET_DIR)/opt/firefox/.[!.]*
	$(INSTALL) -D -m 0644 $(HEXA_FIREFOX_PKGDIR)/policies.json \
		$(TARGET_DIR)/opt/firefox/distribution/policies.json
	ln -sf ../../opt/firefox/firefox $(TARGET_DIR)/usr/bin/firefox
endef

$(eval $(generic-package))
