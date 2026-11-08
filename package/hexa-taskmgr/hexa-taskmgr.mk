################################################################################
#
# hexa-taskmgr: the hexaOS Task Manager (GTK 3)
#
################################################################################

HEXA_TASKMGR_VERSION = 1.0
HEXA_TASKMGR_SITE = $(BR2_EXTERNAL_HEXAOS_PATH)/package/hexa-taskmgr/src
HEXA_TASKMGR_SITE_METHOD = local
HEXA_TASKMGR_LICENSE = MIT
HEXA_TASKMGR_DEPENDENCIES = libgtk3 host-pkgconf

define HEXA_TASKMGR_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define HEXA_TASKMGR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hexa-taskmgr $(TARGET_DIR)/usr/bin/hexa-taskmgr
endef

$(eval $(generic-package))
