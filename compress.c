// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Corellium LLC
 */

#include <linux/slab.h>
#include <linux/zlib.h>
#include <linux/mutex.h>

#include "apfs.h"
#include "libzbitmap.h"
#include "lzfse/lzfse.h"
#include "lzfse/lzvn_decode_base.h"

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

/*
 * Maximum size of compressed data currently supported. This is getting too
 * big already. TODO: avoid vmalloc().
 */
#define MAX_FBUF_SIZE		(256 * 1024 * 1024)

struct apfs_compress_file_data {
	struct apfs_compress_hdr hdr;
	loff_t size;
	void *data;
	u8 *buf;
	ssize_t bufblk;
	size_t bufsize;
	struct mutex mtx;
	struct super_block *sb;
};

static inline int apfs_compress_is_rsrc(u32 algo)
{
	return (algo & 1) == 0;
}

static inline bool apfs_compress_is_supported(u32 algo)
{
	switch(algo) {
	case APFS_COMPRESS_ZLIB_RSRC:
	case APFS_COMPRESS_ZLIB_ATTR:
	case APFS_COMPRESS_LZVN_RSRC:
	case APFS_COMPRESS_LZVN_ATTR:
	case APFS_COMPRESS_PLAIN_RSRC:
	case APFS_COMPRESS_PLAIN_ATTR:
	case APFS_COMPRESS_LZFSE_RSRC:
	case APFS_COMPRESS_LZFSE_ATTR:
	case APFS_COMPRESS_LZBITMAP_RSRC:
	case APFS_COMPRESS_LZBITMAP_ATTR:
		return true;
	default:
		/* Once will usually be enough, don't flood the console */
		pr_err_once("APFS: unsupported compression algorithm (%u)\n", algo);
		return false;
	}
}

static int apfs_compress_file_open(struct inode *inode, struct file *filp)
{
	struct super_block *sb = inode->i_sb;
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
	fd->sb = sb;

	res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &fd->hdr, sizeof(fd->hdr), 0);
	if(res != sizeof(fd->hdr)) {
		apfs_err(sb, "decmpfs header read failed");
		goto fail;
	}

	if(!apfs_compress_is_supported(le32_to_cpu(fd->hdr.algo))) {
		res = -EOPNOTSUPP;
		goto fail;
	}

	if(apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo))) {
		fd->buf = kvmalloc(APFS_COMPRESS_BLOCK, GFP_KERNEL);
		if(!fd->buf)
			goto fail_enomem;
		fd->bufblk = -1;

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_RSRC_FORK, NULL, 0, 0);
		if(res < 0) {
			apfs_err(sb, "fork size read failed");
			goto fail;
		}

		if(res > MAX_FBUF_SIZE) {
			apfs_err(sb, "fork too big");
			goto fail_enomem;
		}

		fd->size = res;
		fd->data = kvmalloc(res, GFP_KERNEL);
		if(!fd->data)
			goto fail_enomem;

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_RSRC_FORK, fd->data, fd->size, 1);
		if(res != fd->size) {
			apfs_err(sb, "fork read failed");
			goto fail;
		}
	} else {
		if(le64_to_cpu(fd->hdr.size) > MAX_FBUF_SIZE) {
			apfs_err(sb, "decmpfs too big");
			goto fail_enomem;
		}

		fd->size = le64_to_cpu(fd->hdr.size);
		fd->data = kvmalloc(le64_to_cpu(fd->hdr.size), GFP_KERNEL);
		if(!fd->data)
			goto fail_enomem;

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, NULL, 0, 0);
		if(res < sizeof(fd->hdr) + 1) {
			apfs_err(sb, "decmpfs size read failed");
			goto fail;
		}
		csize = res - sizeof(fd->hdr);
		if(res > MAX_FBUF_SIZE) {
			apfs_err(sb, "decmpfs too big");
			goto fail_enomem;
		}

		tmp = kvmalloc(res, GFP_KERNEL);
		if(!tmp)
			goto fail_enomem;
		cdata = tmp + sizeof(fd->hdr);

		res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, tmp, csize + sizeof(fd->hdr), 1);
		if(res != csize + sizeof(fd->hdr)) {
			apfs_err(sb, "decmpfs read failed");
			goto fail;
		}

		switch(le32_to_cpu(fd->hdr.algo)) {
		case APFS_COMPRESS_ZLIB_ATTR:
			if(cdata[0] == 0x78 && csize >= 2) {
				res = zlib_inflate_blob(fd->data, fd->size, cdata + 2, csize - 2);
				if(res != fd->size) {
					apfs_err(sb, "zlib attr read failed");
					goto fail;
				}
			} else if((cdata[0] & 0x0F) == 0x0F) {
				if(csize - 1 != fd->size) {
					apfs_err(sb, "zlib attr read failed");
					goto fail_einval;
				}
				memcpy(fd->data, cdata + 1, csize - 1);
			} else {
				apfs_err(sb, "zlib attr read failed");
				goto fail_einval;
			}
			break;
		case APFS_COMPRESS_LZVN_ATTR:
			if(cdata[0] == 0x06) {
				if(csize - 1 != fd->size) {
					apfs_err(sb, "lzvn attr read failed");
					goto fail_einval;
				}
				memcpy(fd->data, cdata + 1, csize - 1);
			} else {
				lzvn_decoder_state dstate = {0};

				dstate.src = cdata;
				dstate.src_end = dstate.src + csize;
				dstate.dst = dstate.dst_begin = fd->data;
				dstate.dst_end = dstate.dst + fd->size;
				lzvn_decode(&dstate);
				if(dstate.dst != fd->data + fd->size) {
					apfs_err(sb, "lzvn attr read failed");
					goto fail_einval;
				}
			}
			break;
		case APFS_COMPRESS_LZBITMAP_ATTR:
			if(cdata[0] == 0x5a) {
				size_t out_len;
				res = zbm_decompress(fd->data, fd->size, cdata, csize, &out_len);
				if(res < 0) {
					apfs_err(sb, "lzbitmap attr read failed");
					goto fail;
				}
				if(out_len != fd->size) {
					apfs_err(sb, "lzbitmap attr read failed");
					goto fail_einval;
				}
			} else if((cdata[0] & 0x0F) == 0x0F) {
				if(csize - 1 != fd->size) {
					apfs_err(sb, "lzbitmap attr read failed");
					goto fail_einval;
				}
				memcpy(fd->data, cdata + 1, csize - 1);
			} else {
				apfs_err(sb, "lzbitmap attr read failed");
				goto fail_einval;
			}
			break;
		case APFS_COMPRESS_LZFSE_ATTR:
			if(cdata[0] == 0x62 && csize >= 2) {
				res = lzfse_decode_buffer(fd->data, fd->size, cdata, csize, NULL);
				if(res != fd->size) {
					apfs_err(sb, "lzfse attr read failed");
					/* Could be ENOMEM too... */
					goto fail_einval;
				}
			} else {
				/*
				 * I've never encountered this, but I assume
				 * it's like the resource version.
				 */
				if(csize - 1 != fd->size) {
					apfs_err(sb, "lzfse attr read failed");
					goto fail_einval;
				}
				memcpy(fd->data, cdata + 1, csize - 1);
			}
			break;
		case APFS_COMPRESS_PLAIN_ATTR:
			if(csize - 1 != fd->size) {
				apfs_err(sb, "plain attr read failed");
				goto fail_einval;
			}
			memcpy(fd->data, cdata + 1, csize - 1);
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
	struct super_block *sb = fd->sb;
	u32 doffs = 0, coffs;
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
		if(le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZBITMAP_RSRC &&
		   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZVN_RSRC &&
		   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZFSE_RSRC) {
			struct apfs_compress_rsrc_hdr *hdr = fd->data;
			struct apfs_compress_rsrc_data *cd = NULL;

			if(fd->size < sizeof(*hdr)) {
				apfs_err(sb, "resource header doesn't fit");
				return -EINVAL;
			}
			doffs = be32_to_cpu(hdr->data_offs);
			if(doffs >= fd->size || fd->size - doffs < sizeof(*cd)) {
				apfs_err(sb, "invalid data header offset in resource");
				return -EINVAL;
			}
			cd = fd->data + doffs;
			if(fd->size - doffs - sizeof(*cd) < sizeof(cd->block[0]) * (size_t)le32_to_cpu(cd->num)) {
				apfs_err(sb, "resource data header won't fit");
				return -EINVAL;
			}

			if(block >= le32_to_cpu(cd->num))
				return 0;

			bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
			if(bsize > APFS_COMPRESS_BLOCK)
				bsize = APFS_COMPRESS_BLOCK;

			csize = le32_to_cpu(cd->block[block].size);
			coffs = le32_to_cpu(cd->block[block].offs) + 4;
		} else {
			__le32 *block_offs = fd->data + doffs;

			if((size_t)fd->size < sizeof(*block_offs) * ((size_t)block + 2)) {
				apfs_err(sb, "resource data header won't fit");
				return -EINVAL;
			}

			bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
			if(bsize > APFS_COMPRESS_BLOCK)
				bsize = APFS_COMPRESS_BLOCK;

			coffs = le32_to_cpu(block_offs[block]);
			csize = le32_to_cpu(block_offs[block + 1]) - coffs;
		}
		if(coffs >= fd->size - doffs || fd->size - doffs - coffs < csize || csize > APFS_COMPRESS_BLOCK + 1) {
			apfs_err(sb, "invalid block size/offset in resource");
			return -EINVAL;
		}
		cdata = fd->data + doffs + coffs;

		switch(le32_to_cpu(fd->hdr.algo)) {
		case APFS_COMPRESS_ZLIB_RSRC:
			if(cdata[0] == 0x78 && csize >= 2) {
				res = zlib_inflate_blob(tmp, bsize, cdata + 2, csize - 2);
				if(res <= 0) {
					apfs_err(sb, "zlib rsrc read failed");
					return res;
				}
				bsize = res;
			} else if((cdata[0] & 0x0F) == 0x0F) {
				memcpy(tmp, &cdata[1], csize - 1);
				bsize = csize - 1;
			} else {
				apfs_err(sb, "zlib rsrc read failed");
				return -EINVAL;
			}
			break;
		case APFS_COMPRESS_LZVN_RSRC:
			if(cdata[0] == 0x06) {
				memcpy(tmp, &cdata[1], csize - 1);
				bsize = csize - 1;
			} else {
				lzvn_decoder_state dstate = {0};

				dstate.src = cdata;
				dstate.src_end = dstate.src + csize;
				dstate.dst = dstate.dst_begin = tmp;
				dstate.dst_end = dstate.dst + bsize;
				lzvn_decode(&dstate);
				bsize = dstate.dst - tmp;
			}
			break;
		case APFS_COMPRESS_LZBITMAP_RSRC:
			if(cdata[0] == 0x5a) {
				res = zbm_decompress(tmp, bsize, cdata, csize, &bsize);
				if(res < 0) {
					apfs_err(sb, "lzbitmap rsrc read failed");
					return res;
				}
			} else if((cdata[0] & 0x0F) == 0x0F) {
				memcpy(tmp, &cdata[1], csize - 1);
				bsize = csize - 1;
			} else {
				apfs_err(sb, "lzbitmap rsrc read failed");
				return -EINVAL;
			}
			break;
		case APFS_COMPRESS_LZFSE_RSRC:
			if(cdata[0] == 0x62 && csize >= 2) {
				res = lzfse_decode_buffer(tmp, bsize, cdata, csize, NULL);
				if(res == 0) {
					apfs_err(sb, "lzfse rsrc read failed");
					/* Could be ENOMEM too... */
					return -EINVAL;
				}
				bsize = res;
			} else {
				/* cdata[0] == 0xff, apparently */
				memcpy(tmp, &cdata[1], csize - 1);
				bsize = csize - 1;
			}
			break;
		case APFS_COMPRESS_PLAIN_RSRC:
			memcpy(tmp, &cdata[1], csize - 1);
			bsize = csize - 1;
			break;
		default:
			return -EINVAL;
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

	if(res < 0)
		return res;
	if(res != sizeof(hdr)) {
		apfs_err(inode->i_sb, "decmpfs header read failed");
		return 1;
	}

	if(!apfs_compress_is_supported(le32_to_cpu(hdr.algo)))
		return 1;

	*size = le64_to_cpu(hdr.size);
	return 0;
}
