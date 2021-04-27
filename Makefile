# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the out-of-tree Linux APFS module.
#

KERNELRELEASE ?= $(shell uname -r)

obj-m = apfs.o
apfs-y := btree.o compress.o dir.o extents.o file.o inode.o key.o message.o \
	  namei.o node.o object.o spaceman.o super.o symlink.o transaction.o \
	  unicode.o xattr.o xfield.o

KDIR ?= /lib/modules/$(KERNELRELEASE)/build

default: modules
	$(MAKE) -C $(KDIR) M=$$PWD modules EXTRA_CFLAGS="-g -DDEBUG"
modules:
	$(MAKE) -C $(KDIR) M=$$PWD EXTRA_CFLAGS="-g -DDEBUG"
clean:
	$(RM) -f *.o *.a *.ko .tmp* .*.*.cmd core Module.symvers *.mod.c modules.order .tmp_versions .depend
install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install
	$(DEPMOD)
