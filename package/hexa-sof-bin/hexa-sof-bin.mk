################################################################################
#
# hexa-sof-bin: firmware de audio Intel SOF (Meteor Lake)
#
################################################################################

HEXA_SOF_BIN_VERSION = 2026.09.2
HEXA_SOF_BIN_SOURCE = sof-bin-$(HEXA_SOF_BIN_VERSION).tar.gz
HEXA_SOF_BIN_SITE = https://github.com/thesofproject/sof-bin/releases/download/v$(HEXA_SOF_BIN_VERSION)
HEXA_SOF_BIN_LICENSE = BSD-3-Clause (firmware), Intel (binarios assinados)
HEXA_SOF_BIN_LICENSE_FILES = LICENCE.Intel
HEXA_SOF_BIN_INSTALL_STAGING = NO

HEXA_SOF_BIN_FW_DIR = $(TARGET_DIR)/lib/firmware/intel

define HEXA_SOF_BIN_INSTALL_TARGET_CMDS
	mkdir -p $(HEXA_SOF_BIN_FW_DIR)/sof-ipc4 $(HEXA_SOF_BIN_FW_DIR)/sof-ipc4-lib \
		$(HEXA_SOF_BIN_FW_DIR)/sof-ipc4-tplg
	cp -a $(@D)/sof-ipc4/mtl $(HEXA_SOF_BIN_FW_DIR)/sof-ipc4/
	cp -a $(@D)/sof-ipc4-lib/mtl $(HEXA_SOF_BIN_FW_DIR)/sof-ipc4-lib/
	cp -a $(@D)/sof-ace-tplg $(HEXA_SOF_BIN_FW_DIR)/
	cp -a $(@D)/sof-ipc4-tplg/sof-mtl-* $(HEXA_SOF_BIN_FW_DIR)/sof-ipc4-tplg/
	cp -a $(@D)/sof-ipc4-tplg/sof-hda-generic* $(HEXA_SOF_BIN_FW_DIR)/sof-ipc4-tplg/ \
		2>/dev/null || true
endef

$(eval $(generic-package))
