include $(sort $(wildcard $(BR2_EXTERNAL_HEXAOS_PATH)/package/*/*.mk))

# Wi-Fi firmware for Meteor Lake (Intel Core Ultra) laptops. There the AX211 is
# built into the chipset (CNVi, "ma" MAC), so iwlwifi asks for
# iwlwifi-ma-b0-gf-a0-*.ucode instead of the so-a0 files of the M.2 card, and
# Buildroot's "iwlwifi 6/6E" option does not install it. Without it no wlan0 appears.
# The list is only expanded when linux-firmware installs, so appending here works
# (after changing it: make linux-firmware-dirclean all).
LINUX_FIRMWARE_FILES += intel/iwlwifi/iwlwifi-ma-*.{ucode,pnvm}

# LLVM, Clang and the SPIR-V/libclc tools built on them (Mesa dependencies) need
# ~1 GB of RAM per C++ compiler process: with all CPUs at once the WSL build runs
# out of memory (cc1plus gets killed). Cap only these packages.
# The Intel video driver (intel-mediadriver) has the same problem: several of
# its files need over 1 GB each to compile.
HEXA_LLVM_JOBS = -j5
HEXA_LLVM_PKGS = LLVM CLANG SPIRV_LLVM_TRANSLATOR SPIRV_TOOLS LIBCLC INTEL_MEDIADRIVER
$(foreach p,$(HEXA_LLVM_PKGS),	$(eval $(p)_BUILD_OPTS += $(HEXA_LLVM_JOBS))	$(eval HOST_$(p)_BUILD_OPTS += $(HEXA_LLVM_JOBS)))
