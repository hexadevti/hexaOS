################################################################################
#
# hexa-files: the hexaOS file manager (GTK 3)
#
################################################################################

HEXA_FILES_VERSION = 1.0
HEXA_FILES_SITE = $(BR2_EXTERNAL_HEXAOS_PATH)/package/hexa-files/src
HEXA_FILES_SITE_METHOD = local
HEXA_FILES_LICENSE = MIT
HEXA_FILES_DEPENDENCIES = libgtk3 host-pkgconf

define HEXA_FILES_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define HEXA_FILES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hexa-files $(TARGET_DIR)/usr/bin/hexa-files
	$(INSTALL) -D -m 0644 $(@D)/hexa-files.desktop \
		$(TARGET_DIR)/usr/share/applications/hexa-files.desktop
endef

$(eval $(generic-package))
