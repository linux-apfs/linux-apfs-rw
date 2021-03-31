# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the out-of-tree Linux APFS module.
#

KERNELRELEASE ?= $(shell uname -r)

obj-m = apfs.o
apfs-y = btree.o dir.o extents.o file.o inode.o key.o message.o \
	 namei.o node.o object.o super.o symlink.o unicode.o xattr.o

default:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(shell pwd)
clean:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(shell pwd) clean
