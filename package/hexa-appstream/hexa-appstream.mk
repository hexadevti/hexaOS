################################################################################
#
# hexa-appstream: AppStream library (Flatpak dependency)
#
################################################################################

HEXA_APPSTREAM_VERSION = 1.2.2
HEXA_APPSTREAM_SOURCE = v$(HEXA_APPSTREAM_VERSION).tar.gz
HEXA_APPSTREAM_SITE = https://github.com/ximion/appstream/archive/refs/tags
HEXA_APPSTREAM_LICENSE = LGPL-2.1+
HEXA_APPSTREAM_LICENSE_FILES = COPYING
HEXA_APPSTREAM_INSTALL_STAGING = YES
HEXA_APPSTREAM_DEPENDENCIES = \
	host-pkgconf host-gperf host-libglib2 \
	libglib2 libcurl libxml2 libxmlb zstd hexa-libfyaml

HEXA_APPSTREAM_CONF_OPTS = \
	-Dstemming=false \
	-Dsystemd=false \
	-Dvapi=false \
	-Dqt=false \
	-Dcompose=false \
	-Dapt-support=false \
	-Dgir=false \
	-Dtools=false \
	-Dzstd-support=true \
	-Dblake3-support=false \
	-Ddisplay-detection=none \
	-Dbash-completion=false \
	-Ddocs=false \
	-Dapidocs=false \
	-Dinstall-docs=false \
	-Dman=false

# When cross-compiling, data/ needs a second AppStream built for the build
# machine (appstreamcli) just to make its own metainfo file. Skip data/,
# translations and tests: the library does not need them.
define HEXA_APPSTREAM_SKIP_EXTRAS
	$(SED) "/^subdir('\(po\|data\|tests\)\/')/d" $(@D)/meson.build
endef
HEXA_APPSTREAM_POST_PATCH_HOOKS += HEXA_APPSTREAM_SKIP_EXTRAS

$(eval $(meson-package))
