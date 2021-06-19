# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the out-of-tree Linux APFS module.
#

KERNELRELEASE ?= $(shell uname -r)
KERNEL_DIR    ?= /lib/modules/$(KERNELRELEASE)/build
PWD           := $(shell pwd)

obj-m = apfs.o
apfs-y := btree.o compress.o dir.o extents.o file.o inode.o key.o message.o \
	  namei.o node.o object.o spaceman.o super.o symlink.o transaction.o \
	  unicode.o xattr.o xfield.o

default:
	make -C $(KERNEL_DIR) M=$(PWD)
install:
	make -C $(KERNEL_DIR) M=$(PWD) modules_install
clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
