################################################################################
#
# hexa-portal: desktop portal for Flatpak apps (open links and files)
#
################################################################################

HEXA_PORTAL_VERSION = 1.0
HEXA_PORTAL_SITE = $(BR2_EXTERNAL_HEXAOS_PATH)/package/hexa-portal/src
HEXA_PORTAL_SITE_METHOD = local
HEXA_PORTAL_LICENSE = MIT
HEXA_PORTAL_DEPENDENCIES = libglib2 host-pkgconf

define HEXA_PORTAL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define HEXA_PORTAL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hexa-portal \
		$(TARGET_DIR)/usr/libexec/hexa-portal
	$(INSTALL) -D -m 0644 $(@D)/org.freedesktop.portal.Desktop.service \
		$(TARGET_DIR)/usr/share/dbus-1/services/org.freedesktop.portal.Desktop.service
endef

$(eval $(generic-package))
