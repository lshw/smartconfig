include $(TOPDIR)/rules.mk

PKG_NAME:=smartconfig
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=liushiwei
PKG_LICENSE:=MIT

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/smartconfig
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Raw 802.11 SmartConfig sender
  DEPENDS:=+libc
endef

define Package/smartconfig/description
  A small C sender for ESP-Touch provisioning over UDP broadcast. It can infer
  the outgoing interface and local address, and can listen for Espressif's
  SmartConfig ACK on UDP port 18266.
endef

define Build/Prepare
	$(RM) -r $(PKG_BUILD_DIR)
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/src
	$(CP) $(CURDIR)/../src/smartconfig.c $(PKG_BUILD_DIR)/src/
	$(CP) $(CURDIR)/../Makefile $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra -std=c11" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/smartconfig/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/smartconfig $(1)/usr/bin/smartconfig
endef

$(eval $(call BuildPackage,smartconfig))
