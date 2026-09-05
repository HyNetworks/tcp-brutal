KERNEL_RELEASE  ?= $(shell uname -r)
KERNEL_DIR      ?= /lib/modules/$(KERNEL_RELEASE)/build
# Modules for a clang-built kernel (CONFIG_CC_IS_CLANG) must be built with LLVM=1
KERNEL_CONFIG   := $(firstword $(wildcard $(KERNEL_DIR)/include/config/auto.conf $(KERNEL_DIR)/.config))
KBUILD_LLVM     := $(if $(KERNEL_CONFIG),$(if $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KERNEL_CONFIG) && echo y),LLVM=1))
DKMS_TARBALL    ?= dkms.tar.gz
TAR             ?= tar
CLANG_FORMAT    ?= clang-format-18
SRCS            := brutal.h brutal_cc.c brutal_sockopt.c brutal_rules.c tools/brutalctl.c tools/Makefile .clang-format
FORMAT_SRCS     := $(filter %.c %.h,$(SRCS))
obj-m           += brutal.o
brutal-objs     := brutal_cc.o brutal_sockopt.o brutal_rules.o

ccflags-y := -std=gnu99

# Kernels with the BBRv3 patchset (XanMod and others) replace the min_tso_segs
# hook with tso_segs(sk, mss_now), which returns the burst size instead of a
# floor. No version macro covers it, so look at the headers being built against.
TCP_H := $(or $(srctree),$(KERNEL_DIR))/include/net/tcp.h
ifneq ($(shell grep -Eq '\(\*tso_segs\)\(struct sock \*sk, unsigned int mss_now\)' $(TCP_H) 2>/dev/null && echo y),)
ccflags-y += -DBRUTAL_HAVE_TSO_SEGS
endif

.PHONY: all clean load unload
.PHONY: .always-make

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $(KBUILD_LLVM) modules

clean: clean-dkms.conf clean-dkms-tarball
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $(KBUILD_LLVM) clean

load:
	sudo insmod brutal.ko

unload:
	sudo rmmod brutal

.PHONY: format format-check
format:
	$(CLANG_FORMAT) --style=file -i $(FORMAT_SRCS)

format-check:
	$(CLANG_FORMAT) --style=file --dry-run --Werror $(FORMAT_SRCS)

.PHONY: dkms-tarball clean-dkms-tarball clean-dkms.conf

.always.make:

dkms.conf: ./scripts/mkdkmsconf.sh .always-make
	./scripts/mkdkmsconf.sh > dkms.conf

clean-dkms.conf:
	$(RM) dkms.conf

$(DKMS_TARBALL): dkms.conf Makefile $(SRCS)
	$(TAR) zcf $(DKMS_TARBALL) \
		--transform 's,^,./dkms_source_tree/,' \
		dkms.conf \
		Makefile \
		$(SRCS)

dkms-tarball: $(DKMS_TARBALL)

clean-dkms-tarball:
	$(RM) $(DKMS_TARBALL)
