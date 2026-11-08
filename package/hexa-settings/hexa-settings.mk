################################################################################
#
# hexa-settings: the hexaOS Settings app (GTK 3)
#
################################################################################

HEXA_SETTINGS_VERSION = 1.0
HEXA_SETTINGS_SITE = $(BR2_EXTERNAL_HEXAOS_PATH)/package/hexa-settings/src
HEXA_SETTINGS_SITE_METHOD = local
HEXA_SETTINGS_LICENSE = MIT
HEXA_SETTINGS_DEPENDENCIES = libgtk3 host-pkgconf

define HEXA_SETTINGS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define HEXA_SETTINGS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hexa-settings $(TARGET_DIR)/usr/bin/hexa-settings
	$(INSTALL) -D -m 0755 $(@D)/hexa-settings-helper \
		$(TARGET_DIR)/usr/libexec/hexa-settings-helper
endef

$(eval $(generic-package))
