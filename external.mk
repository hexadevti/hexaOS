include $(sort $(wildcard $(BR2_EXTERNAL_HEXAOS_PATH)/package/*/*.mk))

# LLVM, Clang and the SPIR-V/libclc tools built on them (Mesa dependencies) need
# ~1 GB of RAM per C++ compiler process: with all CPUs at once the WSL build runs
# out of memory (cc1plus gets killed). Cap only these packages.
HEXA_LLVM_JOBS = -j5
HEXA_LLVM_PKGS = LLVM CLANG SPIRV_LLVM_TRANSLATOR SPIRV_TOOLS LIBCLC
$(foreach p,$(HEXA_LLVM_PKGS),	$(eval $(p)_BUILD_OPTS += $(HEXA_LLVM_JOBS))	$(eval HOST_$(p)_BUILD_OPTS += $(HEXA_LLVM_JOBS)))
