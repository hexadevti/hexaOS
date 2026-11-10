################################################################################
#
# hexa-libfyaml: YAML library (AppStream dependency, for Flatpak)
#
################################################################################

HEXA_LIBFYAML_VERSION = 0.9.6
HEXA_LIBFYAML_SOURCE = libfyaml-$(HEXA_LIBFYAML_VERSION).tar.gz
HEXA_LIBFYAML_SITE = https://github.com/pantoniou/libfyaml/releases/download/v$(HEXA_LIBFYAML_VERSION)
HEXA_LIBFYAML_LICENSE = MIT
HEXA_LIBFYAML_LICENSE_FILES = LICENSE
HEXA_LIBFYAML_INSTALL_STAGING = YES
HEXA_LIBFYAML_DEPENDENCIES = host-pkgconf

# No documentation (Sphinx) or test tools from the build machine; no libclang
# (only for the reflection extras); generic CPU code (portable target)
HEXA_LIBFYAML_CONF_ENV = ac_cv_path_SPHINX= ac_cv_path_JQ=
HEXA_LIBFYAML_CONF_OPTS = \
	--with-libclang=no \
	--disable-network \
	--enable-portable-target

$(eval $(autotools-package))
