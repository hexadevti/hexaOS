################################################################################
#
# hexa-flatpak: Flatpak (sandboxed apps from Flathub, e.g. Steam)
#
################################################################################

HEXA_FLATPAK_VERSION = 1.18.4
HEXA_FLATPAK_SOURCE = flatpak-$(HEXA_FLATPAK_VERSION).tar.xz
HEXA_FLATPAK_SITE = https://github.com/flatpak/flatpak/releases/download/$(HEXA_FLATPAK_VERSION)
HEXA_FLATPAK_LICENSE = LGPL-2.1+
HEXA_FLATPAK_LICENSE_FILES = COPYING
HEXA_FLATPAK_DEPENDENCIES = \
	host-bison host-pkgconf host-libglib2 host-python3 host-python-pyparsing \
	hexa-appstream bubblewrap xdg-dbus-proxy hexa-libostree libgpgme libarchive \
	libcap libcurl libfuse3 libglib2 libseccomp libxml2 json-glib gdk-pixbuf zstd

# Uses Buildroot's bubblewrap/xdg-dbus-proxy instead of its bundled copies.
# No system helper (needs polkit), systemd, parental controls, dconf, X11
# cookies or docs.
HEXA_FLATPAK_CONF_OPTS = \
	-Dsystem_bubblewrap=/usr/bin/bwrap \
	-Dsystem_dbus_proxy=/usr/bin/xdg-dbus-proxy \
	-Dsystem_fusermount=/usr/bin/fusermount3 \
	-Dsystem_helper=disabled \
	-Dsystemd=disabled \
	-Dmalcontent=disabled \
	-Ddconf=disabled \
	-Dxauth=disabled \
	-Dwayland_security_context=disabled \
	-Dseccomp=enabled \
	-Dlibzstd=enabled \
	-Dselinux_module=disabled \
	-Dgir=disabled \
	-Dgtkdoc=disabled \
	-Dman=disabled \
	-Ddocbook_docs=disabled \
	-Dtests=false \
	-Dinstalled_tests=false

# Flathub (the app store Steam comes from) for system-wide installs: root's
# "flatpak install flathub ..." finds it without a remote-add. Per-user
# installs add it in the Store. (File from https://dl.flathub.org/repo/flathub.flatpakrepo)
define HEXA_FLATPAK_INSTALL_FLATHUB
	$(INSTALL) -D -m 0644 $(HEXA_FLATPAK_PKGDIR)/flathub.flatpakrepo \
		$(TARGET_DIR)/etc/flatpak/remotes.d/flathub.flatpakrepo
endef
HEXA_FLATPAK_POST_INSTALL_TARGET_HOOKS += HEXA_FLATPAK_INSTALL_FLATHUB

$(eval $(meson-package))
