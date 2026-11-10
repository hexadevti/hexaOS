################################################################################
#
# hexa-store: the hexaOS Store (GTK 3), built into the system
#
################################################################################

# The same source as the Flatpak app apps/com.hexaos.Store
HEXA_STORE_VERSION = 1.0
HEXA_STORE_SITE = $(BR2_EXTERNAL_HEXAOS_PATH)/apps/com.hexaos.Store/src
HEXA_STORE_SITE_METHOD = local
HEXA_STORE_LICENSE = MIT
HEXA_STORE_DEPENDENCIES = libgtk3 hexa-flatpak host-pkgconf

define HEXA_STORE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define HEXA_STORE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hexa-store $(TARGET_DIR)/usr/bin/hexa-store
endef

$(eval $(generic-package))
