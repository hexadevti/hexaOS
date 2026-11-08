################################################################################
#
# hexa-max98390: MAX98390 speaker amplifier driver (Samsung Galaxy Book4/5)
#
################################################################################

# Pinned commit of Andycodeman/samsung-galaxy-book-linux-fixes (2026-11-05)
HEXA_MAX98390_VERSION = 1b753288a2f4eadc6dd85aa37fecae10661d1b46
HEXA_MAX98390_SITE = $(call github,Andycodeman,samsung-galaxy-book-linux-fixes,$(HEXA_MAX98390_VERSION))
HEXA_MAX98390_LICENSE = GPL-2.0
HEXA_MAX98390_LICENSE_FILES = LICENSE

# Builds snd-hda-scodec-max98390.ko and snd-hda-scodec-max98390-i2c.ko
HEXA_MAX98390_MODULE_SUBDIRS = speaker-fix/src

# The boot script (S12speakers) also needs CONFIG_I2C_CHARDEV: see linux.fragment

$(eval $(kernel-module))
$(eval $(generic-package))
