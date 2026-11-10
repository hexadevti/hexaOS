################################################################################
#
# hexa-power: the hexaOS power window (GTK 3)
#
################################################################################

HEXA_POWER_VERSION = 1.0
HEXA_POWER_SITE = $(BR2_EXTERNAL_HEXAOS_PATH)/package/hexa-power/src
HEXA_POWER_SITE_METHOD = local
HEXA_POWER_LICENSE = MIT
HEXA_POWER_DEPENDENCIES = libgtk3 host-pkgconf

define HEXA_POWER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define HEXA_POWER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hexa-power-gui \
		$(TARGET_DIR)/usr/libexec/hexa-power-gui
endef

$(eval $(generic-package))
