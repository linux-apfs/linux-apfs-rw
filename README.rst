===========================
Apple File System for Linux
===========================

The Apple File System (APFS) is the copy-on-write filesystem currently used on
all Apple devices. This module provides a degree of experimental support on
Linux. It's intended to work with all kernel versions since 4.12, but testing
is focused on 5.18 and above.

To help test write support, a set of userland tools is also under development.
The git tree can be retrieved from https://github.com/eafer/apfsprogs.git.

Known limitations
=================

This module is the result of reverse engineering and testing has been limited.
If you make use of the write support, there is a real risk of data corruption,
so mounts are read-only by default. Please report any issues that you find.

Encryption is not yet implemented even in read-only mode, and neither are
fusion drives.

Reporting bugs
==============

If you encounter any problem, the first thing you should do is run (as root)::

	dmesg | grep -i apfs

to see all the error messages. If that doesn't help you, please file a github
issue at https://github.com/eafer/linux-apfs-rw/issues. Or send me an email if
you prefer.

Build
=====

In order to build a module out-of-tree, you will first need the Linux kernel
headers. Depending on your distro, you can get them by running (as root)::

	# Debian/Ubuntu
	apt-get install linux-headers-$(uname -r)

	# Arch/Manjaro
	pacman -Sy linux-headers

	# RHEL/Rocky/CentOS/Fedora
	yum install kernel-headers kernel-devel

Now you can just cd to the linux-apfs-rw directory and run::

	make

The resulting module is the apfs.ko file. Before you can use it you must insert
it into the kernel, as well as its dependencies. Again as root::

	modprobe libcrc32c
	insmod apfs.ko

Mount
=====

Like all filesystems, apfs is mounted with::

	mount [-o options] device dir

where ``device`` is the path to your device file or filesystem image, and
``dir`` is the mount point. The following options are accepted:

============   =================================================================
vol=n	       Volume number to mount. The default is volume 0.

snap=label     Volume snapshot to mount (in read-only mode).

uid=n, gid=n   Override on-disk inode ownership data with given uid/gid.

cknodes	       Verify the checksum on all metadata nodes. Right now this has a
	       severe performance cost, so it's not recommended.

readwrite      Enable the experimental write support. This may corrupt your
	       container.
============   =================================================================

So for instance, if you want to mount volume number 2, and you want the metadata
to be checked, you should run (as root)::

	mount -o cknodes,vol=2 device dir

To unmount it, run::

	umount dir

Credits
=======

Originally written by Ernesto A. Fernández <ernesto@corellium.com>, with
several contributions from Gabriel Krisman Bertazi <krisman@collabora.com>,
Arnaud Ferraris <arnaud.ferraris@collabora.com> and Stan Skowronek
<skylark@disorder.metrascale.com>. For attribution details see the historical
git tree at https://github.com/eafer/linux-apfs.git.

Work was first based on reverse engineering done by others [1]_ [2]_, and later
on the (very incomplete) official specification [3]_. Some parts of the code
imitate the ext2 module, and to a lesser degree xfs, udf, gfs2 and hfsplus.

.. [1] Hansen, K.H., Toolan, F., Decoding the APFS file system, Digital
   Investigation (2017), https://dx.doi.org/10.1016/j.diin.2017.07.003
.. [2] https://github.com/sgan81/apfs-fuse
.. [3] https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf
