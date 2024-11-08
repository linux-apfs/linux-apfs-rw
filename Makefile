# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the out-of-tree Linux APFS module.
#

KERNELRELEASE ?= $(shell uname -r)
KERNEL_DIR    ?= /lib/modules/$(KERNELRELEASE)/build
PWD           := $(shell pwd)

obj-m = apfs.o
apfs-y := btree.o compress.o dir.o extents.o file.o inode.o key.o libzbitmap.o \
	  lzfse/lzfse_decode.o lzfse/lzfse_decode_base.o lzfse/lzfse_fse.o \
	  lzfse/lzvn_decode_base.o message.o namei.o node.o object.o snapshot.o \
	  spaceman.o super.o symlink.o transaction.o unicode.o xattr.o xfield.o

# If you want mounts to be writable by default, run the build as:
#   make CONFIG=-DCONFIG_APFS_RW_ALWAYS
# This is risky and not generally recommended.
ccflags-y += $(CONFIG)

default:
	./genver.sh
	make -C $(KERNEL_DIR) M=$(PWD)
install:
	make -C $(KERNEL_DIR) M=$(PWD) modules_install
clean:
	rm -f version.h
	make -C $(KERNEL_DIR) M=$(PWD) clean
