################################################################################
#
# hexa-wine: Wine 64-bit (WoW64) para o hexaOS
#
# Baseado no pacote "wine" do Buildroot, que so suporta i386. Aqui o lado Unix
# e 64-bit e as DLLs Windows (32 e 64 bits) sao compiladas com mingw.
#
################################################################################

HEXA_WINE_VERSION = $(WINE_VERSION)
HEXA_WINE_SOURCE = wine-$(HEXA_WINE_VERSION).tar.xz
HEXA_WINE_SITE = $(WINE_SITE)
HEXA_WINE_LICENSE = LGPL-2.1+
HEXA_WINE_LICENSE_FILES = COPYING.LIB LICENSE
HEXA_WINE_DEPENDENCIES = \
	host-bison host-flex host-wine host-pkgconf host-wayland \
	wayland libxkbcommon freetype fontconfig alsa-lib gnutls udev

ifeq ($(BR2_PACKAGE_DBUS),y)
HEXA_WINE_DEPENDENCIES += dbus
HEXA_WINE_CONF_OPTS += --with-dbus
else
HEXA_WINE_CONF_OPTS += --without-dbus
endif

# Compiladores mingw do sistema (pacote gcc-mingw-w64 do Ubuntu)
define HEXA_WINE_CHECK_MINGW
	@for cc in x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc; do \
		command -v $$cc > /dev/null || { \
			echo "ERRO: $$cc nao encontrado. Rode: sudo apt install gcc-mingw-w64"; \
			exit 1; }; \
	done
endef
HEXA_WINE_PRE_CONFIGURE_HOOKS += HEXA_WINE_CHECK_MINGW

HEXA_WINE_CONF_ENV = FREETYPE_CONFIG=$(STAGING_DIR)/usr/bin/freetype-config

# O Buildroot passa --disable-nls (gettext), mas no Wine isso desliga o
# diretorio nls/ (locale.nls, l_intl.nls...), sem o qual o wineserver nao sobe.
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
	--without-opengl \
	--without-oss \
	--without-pcap \
	--without-pcsclite \
	--without-pulse \
	--without-sane \
	--without-sdl \
	--without-usb \
	--without-v4l2 \
	--without-vulkan

# Veja o comentario equivalente em package/wine/wine.mk
ifeq ($(BR2_TOOLCHAIN_EXTERNAL),y)
HEXA_WINE_CONF_OPTS += TARGETFLAGS="-b $(TOOLCHAIN_EXTERNAL_PREFIX)"
endif

# O "make install" nao cria /usr/bin/wine (os outros comandos apontam para ele)
define HEXA_WINE_LINK_LOADER
	ln -sf ../lib/wine/x86_64-unix/wine $(TARGET_DIR)/usr/bin/wine
endef
HEXA_WINE_POST_INSTALL_TARGET_HOOKS += HEXA_WINE_LINK_LOADER

$(eval $(autotools-package))

# O driver Wayland do Wine precisa da libxkbregistry, que o Buildroot desliga
# no libxkbcommon. Religamos aqui (ela depende da libxml2).
ifeq ($(BR2_PACKAGE_HEXA_WINE),y)
LIBXKBCOMMON_CONF_OPTS := \
	$(filter-out -Denable-xkbregistry=false,$(LIBXKBCOMMON_CONF_OPTS)) \
	-Denable-xkbregistry=true
$(LIBXKBCOMMON_TARGET_CONFIGURE): | libxml2
endif
