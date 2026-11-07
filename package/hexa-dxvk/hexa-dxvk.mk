################################################################################
#
# hexa-dxvk: DXVK DLLs (Direct3D -> Vulkan) for Wine
#
################################################################################

HEXA_DXVK_VERSION = 3.1.1
HEXA_DXVK_SOURCE = dxvk-$(HEXA_DXVK_VERSION).tar.gz
HEXA_DXVK_SITE = https://github.com/doitsujin/dxvk/releases/download/v$(HEXA_DXVK_VERSION)
HEXA_DXVK_LICENSE = Zlib

define HEXA_DXVK_INSTALL_TARGET_CMDS
	for a in x32 x64; do \
		$(INSTALL) -d $(TARGET_DIR)/usr/share/dxvk/$$a; \
		$(INSTALL) -m 0644 $(@D)/$$a/*.dll $(TARGET_DIR)/usr/share/dxvk/$$a/; \
	done
	$(INSTALL) -D -m 0755 $(HEXA_DXVK_PKGDIR)/hexa-dxvk $(TARGET_DIR)/usr/bin/hexa-dxvk
endef

$(eval $(generic-package))
