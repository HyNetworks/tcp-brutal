KERNEL_RELEASE  ?= $(shell uname -r)
KERNEL_DIR      ?= /lib/modules/$(KERNEL_RELEASE)/build
DKMS_TARBALL    ?= dkms.tar.gz
TAR             ?= tar
CLANG_FORMAT    ?= clang-format-18
SRCS            := brutal.h brutal_cc.c brutal_sockopt.c brutal_rules.c tools/brutalctl.c tools/Makefile .clang-format
FORMAT_SRCS     := $(filter %.c %.h,$(SRCS))
obj-m           += brutal.o
brutal-objs     := brutal_cc.o brutal_sockopt.o brutal_rules.o

ccflags-y := -std=gnu99

.PHONY: all clean load unload
.PHONY: .always-make

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean: clean-dkms.conf clean-dkms-tarball
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

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
