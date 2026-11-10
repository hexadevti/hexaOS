################################################################################
#
# hexa-libostree: libostree for Flatpak, without composefs (it needs kernel
# headers 5.8; the toolchain has 5.4). Based on Buildroot's libostree.mk.
#
################################################################################

HEXA_LIBOSTREE_VERSION = 2023.8
HEXA_LIBOSTREE_SOURCE = libostree-$(HEXA_LIBOSTREE_VERSION).tar.xz
HEXA_LIBOSTREE_SITE = https://github.com/ostreedev/ostree/releases/download/v$(HEXA_LIBOSTREE_VERSION)
HEXA_LIBOSTREE_LICENSE = LGPL-2.0+
HEXA_LIBOSTREE_LICENSE_FILES = COPYING
HEXA_LIBOSTREE_INSTALL_STAGING = YES
# libcurl: the HTTP backend that downloads from Flathub (without one, ostree
# fails with "built without libsoup or libcurl, and cannot fetch over HTTP")
HEXA_LIBOSTREE_DEPENDENCIES = e2fsprogs host-bison host-pkgconf libcurl libglib2 \
	libgpg-error libgpgme util-linux xz

HEXA_LIBOSTREE_CONF_ENV = \
	GPG_ERROR_CONFIG=$(STAGING_DIR)/usr/bin/gpg-error-config
HEXA_LIBOSTREE_CONF_OPTS = \
	--with-gpgme-prefix=$(STAGING_DIR)/usr \
	--without-composefs \
	--without-soup \
	--without-soup3 \
	--with-curl \
	--without-avahi \
	--without-selinux \
	--without-libsystemd \
	--disable-rofiles-fuse \
	--disable-introspection \
	--disable-gtk-doc \
	--disable-gtk-doc-html \
	--disable-gtk-doc-pdf \
	--disable-man

ifeq ($(BR2_PACKAGE_OPENSSL),y)
HEXA_LIBOSTREE_CONF_OPTS += --with-openssl
HEXA_LIBOSTREE_DEPENDENCIES += openssl
else
HEXA_LIBOSTREE_CONF_OPTS += --without-openssl
endif

ifeq ($(BR2_PACKAGE_LIBARCHIVE),y)
HEXA_LIBOSTREE_CONF_OPTS += --with-libarchive
HEXA_LIBOSTREE_DEPENDENCIES += libarchive
else
HEXA_LIBOSTREE_CONF_OPTS += --without-libarchive
endif

$(eval $(autotools-package))
