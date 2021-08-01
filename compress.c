// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Corellium LLC
 */

#include <linux/slab.h>
#include <linux/zlib.h>
#include <linux/mutex.h>

#include "apfs.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)

#include <linux/vmalloc.h>

static inline void *kvmalloc(size_t size, gfp_t flags)
{
	gfp_t kmalloc_flags = flags;
	void *ret;

	if ((flags & GFP_KERNEL) != GFP_KERNEL)
		return kmalloc(size, flags);

	if (size > PAGE_SIZE)
		kmalloc_flags |= __GFP_NOWARN | __GFP_NORETRY;

	ret = kmalloc(size, flags);
	if (ret || size < PAGE_SIZE)
		return ret;

	return vmalloc(size);
}

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0) */

/* maximum size of compressed data currently supported */
#define MAX_FBUF_SIZE		(128 * 1024 * 1024)

struct apfs_compress_file_data {
	struct apfs_compress_hdr hdr;
	loff_t size;
	void *data;
	u8 *buf;
	ssize_t bufblk;
	size_t bufsize;
	struct mutex mtx;
};

static inline int apfs_compress_is_rsrc(u32 algo)
{
	return (algo == APFS_COMPRESS_ZLIB_RSRC);
}

static int apfs_compress_file_open(struct inode *inode, struct file *filp)
{
	struct apfs_compress_file_data *fd;
	ssize_t res;
	u8 *tmp = NULL, *cdata;
	size_t csize;

	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;

	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	if(!fd)
		return -ENOMEM;
	mutex_init(&fd->mtx);

	res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &fd->hdr, sizeof(fd->hdr), 0);
	if(res != sizeof(fd->hdr))
		goto fail;

	if(apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo))) {
		fd->buf = kvmalloc(APFS_COMPRESS_BLOCK, GFP_KERNEL);
		if(!fd->buf)
			goto fail_enomem;
		fd->bufblk = -1;

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_RSRC_FORK, NULL, 0, 0);
		if(res < 0)
			goto fail;

		if(res > MAX_FBUF_SIZE)
			goto fail_enomem;

		fd->size = res;
		fd->data = kvmalloc(res, GFP_KERNEL);
		if(!fd->data)
			goto fail_enomem;

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_RSRC_FORK, fd->data, fd->size, 1);
		if(res != fd->size)
			goto fail;
	} else {
		if(le64_to_cpu(fd->hdr.size) > MAX_FBUF_SIZE)
			goto fail_enomem;

		fd->size = le64_to_cpu(fd->hdr.size);
		fd->data = kvmalloc(le64_to_cpu(fd->hdr.size), GFP_KERNEL);
		if(!fd->data)
			goto fail_enomem;

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, NULL, 0, 0);
		if(res < sizeof(fd->hdr) + 1)
			goto fail;
		csize = res - sizeof(fd->hdr);
		if(res > MAX_FBUF_SIZE)
			goto fail_enomem;

		tmp = kvmalloc(res, GFP_KERNEL);
		if(!tmp)
			goto fail_enomem;
		cdata = tmp + sizeof(fd->hdr);

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, tmp, csize + sizeof(fd->hdr), 1);
		if(res != csize + sizeof(fd->hdr))
			goto fail;

		switch(le32_to_cpu(fd->hdr.algo)) {
		case APFS_COMPRESS_ZLIB_ATTR:
			if(cdata[0] == 0x78 && csize >= 2) {
				res = zlib_inflate_blob(fd->data, fd->size, cdata + 2, csize - 2);
				if(res != fd->size)
					goto fail;
			} else if((cdata[0] & 0x0F) == 0x0F) {
				if(csize - 1 != fd->size)
					goto fail_einval;
				memcpy(fd->data, cdata + 1, csize - 1);
			} else
				goto fail_einval;
			break;
		default:
			goto fail_einval;
		}

		kvfree(tmp);
	}

	filp->private_data = fd;
	return 0;

fail_enomem:
	res = -ENOMEM;
	goto fail;
fail_einval:
	res = -EINVAL;
fail:
	if(tmp)
		kvfree(tmp);
	if(fd->data)
		kvfree(fd->data);
	if(fd->buf)
		kvfree(fd->buf);
	kfree(fd);
	if(res > 0)
		res = -EINVAL;
	return res;
}

static ssize_t apfs_compress_file_read_block(struct apfs_compress_file_data *fd, char __user *buf, size_t size, loff_t off)
{
	struct apfs_compress_rsrc_hdr *hdr = fd->data;
	u32 doffs, coffs;
	struct apfs_compress_rsrc_data *cd;
	loff_t block;
	u8 *cdata, *tmp = fd->buf;
	size_t csize, bsize;
	ssize_t res;

	if(off >= le64_to_cpu(fd->hdr.size))
		return 0;
	if(size > le64_to_cpu(fd->hdr.size) - off)
		size = le64_to_cpu(fd->hdr.size) - off;

	block = off / APFS_COMPRESS_BLOCK;
	off -= block * APFS_COMPRESS_BLOCK;
	if(block != fd->bufblk) {
		if(fd->size < sizeof(*hdr))
			return -EINVAL;
		doffs = be32_to_cpu(hdr->data_offs);
		if(doffs >= fd->size || fd->size - doffs < sizeof(*cd))
			return -EINVAL;
		cd = fd->data + doffs;
		if(fd->size - doffs - sizeof(*cd) < sizeof(cd->block[0]) * (size_t)le32_to_cpu(cd->num))
			return -EINVAL;

		if(block >= le32_to_cpu(cd->num))
			return 0;

		bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
		if(bsize > APFS_COMPRESS_BLOCK)
			bsize = APFS_COMPRESS_BLOCK;

		csize = le32_to_cpu(cd->block[block].size);
		coffs = le32_to_cpu(cd->block[block].offs) + 4;
		if(coffs >= fd->size - doffs || fd->size - doffs - coffs < csize || csize > APFS_COMPRESS_BLOCK + 1)
			return -EINVAL;
		cdata = fd->data + doffs + coffs;

		switch(le32_to_cpu(fd->hdr.algo)) {
		case APFS_COMPRESS_ZLIB_RSRC:
			if(cdata[0] == 0x78 && csize >= 2) {
				res = zlib_inflate_blob(tmp, bsize, cdata + 2, csize - 2);
				if(res <= 0)
					return res;
				bsize = res;
			} else if((cdata[0] & 0x0F) == 0x0F) {
				memcpy(tmp, &cdata[1], csize - 1);
				bsize = csize - 1;
			} else
				return -EINVAL;
			break;
		}
		fd->bufblk = block;
		fd->bufsize = bsize;
	} else
		bsize = fd->bufsize;

	if(bsize < off)
		return 0;
	bsize -= off;
	if(size > bsize)
		size = bsize;
	res = copy_to_user(buf, tmp + off, size);
	if(res == size)
		return -EFAULT;
	return size - res;
}

static ssize_t apfs_compress_file_read(struct file *filp, char __user *buf, size_t size, loff_t *off)
{
	struct apfs_compress_file_data *fd = filp->private_data;
	loff_t step;
	ssize_t block, res;

	if(apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo))) {
		step = 0;
		while(step < size) {
			block = APFS_COMPRESS_BLOCK - ((*off + step) & (APFS_COMPRESS_BLOCK - 1));
			if(block > size - step)
				block = size - step;
			mutex_lock(&fd->mtx);
			res = apfs_compress_file_read_block(fd, buf + step, block, *off + step);
			mutex_unlock(&fd->mtx);
			if(res < block) {
				if(res < 0 && !step)
					return res;
				step += res > 0 ? res : 0;
				break;
			}
			step += block;
		}
		*off += step;
		return step;
	} else {
		if(*off >= fd->size)
			return 0;
		if(size > fd->size - *off)
			size = fd->size - *off;
		res = copy_to_user(buf, fd->data + *off, size);
		if(res == size)
			return -EFAULT;
		*off += size - res;
		return size - res;
	}
}

static int apfs_compress_file_release(struct inode *inode, struct file *filp)
{
	struct apfs_compress_file_data *fd = filp->private_data;
	if(fd->data)
		kvfree(fd->data);
	if(fd->buf)
		kvfree(fd->buf);
	kfree(fd);
	return 0;
}

const struct file_operations apfs_compress_file_operations = {
	.open		= apfs_compress_file_open,
	.llseek		= generic_file_llseek,
	.read		= apfs_compress_file_read,
	.release	= apfs_compress_file_release,
};

int apfs_compress_get_size(struct inode *inode, loff_t *size)
{
	struct apfs_compress_hdr hdr;
	int res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &hdr, sizeof(hdr), 0);
	u32 algo;

	if(res < 0)
		return res;
	if(res != sizeof(hdr))
		return 1;

	algo = le32_to_cpu(hdr.algo);
	if(algo != APFS_COMPRESS_ZLIB_RSRC &&
	   algo != APFS_COMPRESS_ZLIB_ATTR)
		return 1;

	*size = le64_to_cpu(hdr.size);
	return 0;
}
