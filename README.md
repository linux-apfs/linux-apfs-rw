
# Apple File System (APFS) module for Linux with experimental write support.

## [linux-apfs-rw](https://github.com/linux-apfs/apfsprogs)

An experimental Linux kernel module for working with APFS disks & images.

### Working:
 - Mount APFS with **read-only** permissions on Linux

### Experimental:
 - Mount APFS with **read/write** permissions on Linux **(enabling write support WILL corrupt the container)**

### TODO
 - Encryption.
 - Compression, though the compressed contents of a file can be read from the `com.apple.decmpfs` and `com.apple.ResourceFork` xattrs as long as they are under 64k.
 - Restoring to a snapshot.
 - Access control lists. This is not a priority.

### Related projects

apfsprogs - [https://github.com/linux-apfs/apfsprogs](https://github.com/linux-apfs/apfsprogs)

linux-apfs - [https://github.com/linux-apfs/linux-apfs](https://github.com/linux-apfs/linux-apfs) (previous development.)

## Apple File System (APFS)

The Apple File System (APFS) is the copy-on-write filesystem currently used on
all Apple devices. This module provides a degree of experimental support on
Linux.

It's supposed to work with a range of kernel versions starting at 4.9 or before,
but only a few of those have actually been tested. If you run into any problem,
please send a report to <linux-apfs@googlegroups.com>.

To help test write support, a set of userland tools is also under development.
The git tree can be retrieved from <https://github.com/eafer/apfsprogs.git>.

## Known limitations

This module is the result of reverse engineering and testing has been limited.
You should expect data corruption. Please report any issues that you find.

Apple has released other versions of the filesystem to the public before the
current one. I would not expect them to be compatible with this module at all,
but I am open to fixing that if requested.


## Building

In order to build a module out-of-tree, you will first need the **Linux kernel
headers**. You can get them by running:

```bash
# Debian/Ubuntu
apt-get install "linux-headers-$(uname -r)"

# Arch/Manjaro
pacman -Sy linux-headers

# RHEL/Rocky/CentOS/Fedora
yum install kernel-headers kernel-devel
```

Now you can just cd to the linux-apfs-rw directory and run

```bash
git clone https://github.com/linux-apfs/linux-apfs-rw.git
cd ./linux-apfs-rw
# make clean
make
```

The resulting module is the `apfs.ko` file. Before you can use it you must insert
it into the kernel, as well as its dependencies. Again as root:

```bash
modprobe libcrc32c
insmod apfs.ko

# sudo modprobe libcrc32c
# sudo insmod apfs.ko
```

## Mount APFS on Linux

Like all filesystems, apfs is mounted with

```bash
mount [-o options] device dir
```

where 'device' is the path to your device file or filesystem image, and 'dir'
is the mount point. The following options are accepted:

```console
  vol=n
	Volume number to mount. The default is volume 0.

  uid=n, gid=n
	Override on-disk inode ownership data with given uid/gid.

  cknodes
	Verify the checksum on all metadata nodes. Right now this has
	a severe performance cost, so it's not recommended.

  readwrite
	Enable the experimental write support. This WILL corrupt the container.
```

For example, if you want to mount volume number 2, and you want the metadata
to be checked, you should run (as root):

```bash
mount -o cknodes,vol=2 device dir
```

To unmount it, run
```bash
umount dir
```


## Credits

Originally written by Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>,
with several contributions from Gabriel Krisman Bertazi <krisman@collabora.com>,
Arnaud Ferraris <arnaud.ferraris@collabora.com> and Stan Skowronek
<skylark@disorder.metrascale.com>. For attribution details see the historical
git tree at <https://github.com/eafer/linux-apfs.git>.

Work was first based on reverse engineering done by others [1][2], and later
on the (very incomplete) official specification [3]. Some parts of the code
imitate the ext2 module, and to a lesser degree xfs, udf, gfs2 and hfsplus.

1.  Hansen, K.H., Toolan, F., Decoding the APFS file system, Digital Investigation (2017), http://dx.doi.org/10.1016/j.diin.2017.07.003
2. https://github.com/sgan81/apfs-fuse
3. https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf
