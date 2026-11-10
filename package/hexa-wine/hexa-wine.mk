################################################################################
#
# hexa-wine: 64-bit Wine (WoW64) for hexaOS
#
# Based on Buildroot's "wine" package, which only supports i386. Here the Unix
# side is 64-bit and the Windows DLLs (32 and 64-bit) are built with mingw.
#
################################################################################

HEXA_WINE_VERSION = $(WINE_VERSION)
HEXA_WINE_SOURCE = wine-$(HEXA_WINE_VERSION).tar.xz
HEXA_WINE_SITE = $(WINE_SITE)
HEXA_WINE_LICENSE = LGPL-2.1+
HEXA_WINE_LICENSE_FILES = COPYING.LIB LICENSE
HEXA_WINE_DEPENDENCIES = \
	host-bison host-flex host-wine host-pkgconf host-wayland \
	wayland libxkbcommon freetype fontconfig alsa-lib gnutls udev \
	mesa3d vulkan-loader pulseaudio

ifeq ($(BR2_PACKAGE_DBUS),y)
HEXA_WINE_DEPENDENCIES += dbus
HEXA_WINE_CONF_OPTS += --with-dbus
else
HEXA_WINE_CONF_OPTS += --without-dbus
endif

# System mingw compilers (Ubuntu package gcc-mingw-w64)
define HEXA_WINE_CHECK_MINGW
	@for cc in x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc; do \
		command -v $$cc > /dev/null || { \
			echo "ERROR: $$cc not found. Run: sudo apt install gcc-mingw-w64"; \
			exit 1; }; \
	done
endef
HEXA_WINE_PRE_CONFIGURE_HOOKS += HEXA_WINE_CHECK_MINGW

HEXA_WINE_CONF_ENV = FREETYPE_CONFIG=$(STAGING_DIR)/usr/bin/freetype-config

# NTSYNC (/dev/ntsync): Wine only uses it when linux/ntsync.h exists at build
# time, and that header is newer (6.14) than the toolchain's kernel headers.
# Take it from the kernel we build, in a private include dir so the rest of
# the toolchain headers are not shadowed.
ifeq ($(BR2_LINUX_KERNEL),y)
HEXA_WINE_DEPENDENCIES += linux
define HEXA_WINE_NTSYNC_HEADER
	mkdir -p $(HEXA_WINE_DIR)/hexa-include/linux
	cp $(LINUX_DIR)/include/uapi/linux/ntsync.h $(HEXA_WINE_DIR)/hexa-include/linux/
endef
HEXA_WINE_PRE_CONFIGURE_HOOKS += HEXA_WINE_NTSYNC_HEADER
HEXA_WINE_CONF_ENV += CPPFLAGS="$(TARGET_CPPFLAGS) -I$(HEXA_WINE_DIR)/hexa-include"
endif

# Buildroot passes --disable-nls (gettext), but in Wine that disables the
# nls/ directory (locale.nls, l_intl.nls...), without which wineserver fails.
HEXA_WINE_CONF_OPTS += \
	--with-wine-tools=../host-wine-$(HEXA_WINE_VERSION) \
	--enable-archs=i386,x86_64 \
	--disable-tests \
	--enable-nls \
	--with-wayland \
	--with-freetype \
	--with-fontconfig \
	--with-alsa \
	--with-gnutls \
	--with-udev \
	--without-x \
	--without-capi \
	--without-coreaudio \
	--without-cups \
	--without-ffmpeg \
	--without-gettext \
	--without-gettextpo \
	--without-gphoto \
	--without-gstreamer \
	--without-krb5 \
	--without-netapi \
	--without-opencl \
	--without-oss \
	--without-pcap \
	--without-pcsclite \
	--without-sane \
	--without-sdl \
	--without-usb \
	--without-v4l2 \
	--with-opengl \
	--with-vulkan \
	--with-pulse

# See the equivalent comment in package/wine/wine.mk
ifeq ($(BR2_TOOLCHAIN_EXTERNAL),y)
HEXA_WINE_CONF_OPTS += TARGETFLAGS="-b $(TOOLCHAIN_EXTERNAL_PREFIX)"
endif

# "make install" does not create /usr/bin/wine (the other commands point to it)
define HEXA_WINE_LINK_LOADER
	ln -sf ../lib/wine/x86_64-unix/wine $(TARGET_DIR)/usr/bin/wine
endef
HEXA_WINE_POST_INSTALL_TARGET_HOOKS += HEXA_WINE_LINK_LOADER

$(eval $(autotools-package))

# Wine's Wayland driver needs libxkbregistry, which Buildroot disables in
# libxkbcommon. Re-enable it here (it depends on libxml2).
ifeq ($(BR2_PACKAGE_HEXA_WINE),y)
LIBXKBCOMMON_CONF_OPTS := \
	$(filter-out -Denable-xkbregistry=false,$(LIBXKBCOMMON_CONF_OPTS)) \
	-Denable-xkbregistry=true
$(LIBXKBCOMMON_TARGET_CONFIGURE): | libxml2
endif
