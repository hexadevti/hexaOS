################################################################################
#
# hexa-alsa-ucm-conf: ALSA Use Case Manager profiles (same version as alsa-lib)
#
################################################################################

HEXA_ALSA_UCM_CONF_VERSION = 1.2.15.3
HEXA_ALSA_UCM_CONF_SOURCE = alsa-ucm-conf-$(HEXA_ALSA_UCM_CONF_VERSION).tar.bz2
HEXA_ALSA_UCM_CONF_SITE = https://www.alsa-project.org/files/pub/lib
HEXA_ALSA_UCM_CONF_LICENSE = BSD-3-Clause
HEXA_ALSA_UCM_CONF_LICENSE_FILES = LICENSE

define HEXA_ALSA_UCM_CONF_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/share/alsa
	rm -rf $(TARGET_DIR)/usr/share/alsa/ucm2 $(TARGET_DIR)/usr/share/alsa/ucm
	cp -a $(@D)/ucm2 $(@D)/ucm $(TARGET_DIR)/usr/share/alsa/
	find $(TARGET_DIR)/usr/share/alsa/ucm2 $(TARGET_DIR)/usr/share/alsa/ucm \
		-name README.md -delete
endef

$(eval $(generic-package))
