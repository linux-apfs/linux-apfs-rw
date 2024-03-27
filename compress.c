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

struct apfs_compress_file_data {
	struct apfs_compress_hdr hdr;
	u8 *buf;
	ssize_t bufblk;
	size_t bufsize;
	struct mutex mtx;
	struct super_block *sb;
	struct apfs_compressed_data cdata;
};

static inline int apfs_compress_is_rsrc(u32 algo)
{
	return (algo & 1) == 0;
}

static inline bool apfs_compress_is_supported(u32 algo)
{
	switch (algo) {
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_compress_file_data *fd;
	ssize_t res;
	bool is_rsrc;

	/*
	 * The official implementation seems to transparently decompress files
	 * when you write to them. Doing that atomically inside the kernel is
	 * probably a chore, so for now I'll just leave it to the user to make
	 * an uncompressed copy themselves and replace the original. I might
	 * fix this in the future, but only if people complain (TODO).
	 */
	if (filp->f_mode & FMODE_WRITE) {
		apfs_warn(sb, "writes to compressed files are not supported");
		apfs_warn(sb, "you can work with a copy of the file instead");
		return -EOPNOTSUPP;
	}

	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;

	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;
	mutex_init(&fd->mtx);
	fd->sb = sb;

	down_read(&nxi->nx_big_sem);

	res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &fd->hdr, sizeof(fd->hdr), 0);
	if (res != sizeof(fd->hdr)) {
		apfs_err(sb, "decmpfs header read failed");
		goto fail;
	}

	if (!apfs_compress_is_supported(le32_to_cpu(fd->hdr.algo))) {
		res = -EOPNOTSUPP;
		goto fail;
	}

	fd->buf = kvmalloc(APFS_COMPRESS_BLOCK, GFP_KERNEL);
	if (!fd->buf) {
		res = -ENOMEM;
		goto fail;
	}
	fd->bufblk = -1;

	is_rsrc = apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo));
	res = apfs_xattr_get_compressed_data(inode, is_rsrc ? APFS_XATTR_NAME_RSRC_FORK : APFS_XATTR_NAME_COMPRESSED, &fd->cdata);
	if (res) {
		apfs_err(sb, "failed to get compressed data");
		goto fail;
	}

	up_read(&nxi->nx_big_sem);

	filp->private_data = fd;
	return 0;

fail:
	apfs_release_compressed_data(&fd->cdata);
	if (fd->buf)
		kvfree(fd->buf);
	up_read(&nxi->nx_big_sem);
	kfree(fd);
	if (res > 0)
		res = -EINVAL;
	return res;
}

static int apfs_compress_file_read_block(struct apfs_compress_file_data *fd, loff_t block)
{
	struct super_block *sb = fd->sb;
	struct apfs_compressed_data *comp_data = &fd->cdata;
	u8 *cdata = NULL;
	u8 *tmp = fd->buf;
	u32 doffs = 0, coffs;
	size_t csize, bsize;
	int res = 0;

	if (apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo)) &&
	   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZBITMAP_RSRC &&
	   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZVN_RSRC &&
	   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZFSE_RSRC) {
		struct apfs_compress_rsrc_hdr hdr = {0};
		struct apfs_compress_rsrc_data cd = {0};
		struct apfs_compress_rsrc_block blk = {0};
		u32 blk_off;

		res = apfs_compressed_data_read(comp_data, &hdr, sizeof(hdr), 0 /* offset */);
		if (res) {
			apfs_err(sb, "failed to read resource header");
			return res;
		}

		doffs = be32_to_cpu(hdr.data_offs);
		res = apfs_compressed_data_read(comp_data, &cd, sizeof(cd), doffs);
		if (res) {
			apfs_err(sb, "failed to read resource data header");
			return res;
		}
		if (block >= le32_to_cpu(cd.num))
			return 0;

		blk_off = doffs + sizeof(cd) + sizeof(blk) * block;
		res = apfs_compressed_data_read(comp_data, &blk, sizeof(blk), blk_off);
		if (res) {
			apfs_err(sb, "failed to read resource block metadata");
			return res;
		}

		bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
		if (bsize > APFS_COMPRESS_BLOCK)
			bsize = APFS_COMPRESS_BLOCK;

		csize = le32_to_cpu(blk.size);
		coffs = le32_to_cpu(blk.offs) + 4;
	} else if (apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo))) {
		__le32 blks[2];
		u32 blk_off;

		blk_off = doffs + sizeof(__le32) * block;
		res = apfs_compressed_data_read(comp_data, blks, sizeof(blks), blk_off);
		if (res) {
			apfs_err(sb, "failed to read resource block metadata");
			return res;
		}

		bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
		if (bsize > APFS_COMPRESS_BLOCK)
			bsize = APFS_COMPRESS_BLOCK;

		coffs = le32_to_cpu(blks[0]);
		csize = le32_to_cpu(blks[1]) - coffs;
	} else {
		/*
		 * I think attr compression is only for single-block files, in
		 * fact none of these files ever seem to decompress to more than
		 * 2048 bytes.
		 */
		bsize = le64_to_cpu(fd->hdr.size);
		if (block != 0 || bsize > APFS_COMPRESS_BLOCK) {
			apfs_err(sb, "file too big for inline compression");
			return -EFSCORRUPTED;
		}

		/* The first few bytes are the decmpfs header */
		coffs = sizeof(struct apfs_compress_hdr);
		csize = comp_data->size - sizeof(struct apfs_compress_hdr);
	}

	cdata = kvmalloc(csize, GFP_KERNEL);
	if (!cdata)
		return -ENOMEM;
	res = apfs_compressed_data_read(comp_data, cdata, csize, doffs + coffs);
	if (res) {
		apfs_err(sb, "failed to read compressed block");
		goto fail;
	}

	switch (le32_to_cpu(fd->hdr.algo)) {
	case APFS_COMPRESS_ZLIB_RSRC:
	case APFS_COMPRESS_ZLIB_ATTR:
		if (cdata[0] == 0x78 && csize >= 2) {
			res = zlib_inflate_blob(tmp, bsize, cdata + 2, csize - 2);
			if (res <= 0) {
				apfs_err(sb, "zlib decompression failed");
				goto fail;
			}
			bsize = res;
			res = 0;
		} else if ((cdata[0] & 0x0F) == 0x0F) {
			memcpy(tmp, &cdata[1], csize - 1);
			bsize = csize - 1;
		} else {
			apfs_err(sb, "zlib decompression failed");
			res = -EINVAL;
			goto fail;
		}
		break;
	case APFS_COMPRESS_LZVN_RSRC:
	case APFS_COMPRESS_LZVN_ATTR:
		if (cdata[0] == 0x06) {
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
	case APFS_COMPRESS_LZBITMAP_ATTR:
		if (cdata[0] == 0x5a) {
			res = zbm_decompress(tmp, bsize, cdata, csize, &bsize);
			if (res < 0) {
				apfs_err(sb, "lzbitmap decompression failed");
				goto fail;
			}
			res = 0;
		} else if ((cdata[0] & 0x0F) == 0x0F) {
			memcpy(tmp, &cdata[1], csize - 1);
			bsize = csize - 1;
		} else {
			apfs_err(sb, "lzbitmap decompression failed");
			res = -EINVAL;
			goto fail;
		}
		break;
	case APFS_COMPRESS_LZFSE_RSRC:
	case APFS_COMPRESS_LZFSE_ATTR:
		if (cdata[0] == 0x62 && csize >= 2) {
			res = lzfse_decode_buffer(tmp, bsize, cdata, csize, NULL);
			if (res == 0) {
				apfs_err(sb, "lzfse decompression failed");
				/* Could be ENOMEM too... */
				res = -EINVAL;
				goto fail;
			}
			bsize = res;
			res = 0;
		} else {
			/* cdata[0] == 0xff, apparently */
			memcpy(tmp, &cdata[1], csize - 1);
			bsize = csize - 1;
		}
		break;
	case APFS_COMPRESS_PLAIN_RSRC:
	case APFS_COMPRESS_PLAIN_ATTR:
		memcpy(tmp, &cdata[1], csize - 1);
		bsize = csize - 1;
		break;
	default:
		res = -EINVAL;
		goto fail;
	}
	fd->bufblk = block;
	fd->bufsize = bsize;
fail:
	kvfree(cdata);
	return res;
}

static int apfs_compress_file_release(struct inode *inode, struct file *filp)
{
	struct apfs_compress_file_data *fd = filp->private_data;

	apfs_release_compressed_data(&fd->cdata);
	if (fd->buf)
		kvfree(fd->buf);
	kfree(fd);
	return 0;
}

static ssize_t apfs_compress_file_read_from_block(struct apfs_compress_file_data *fd, char *buf, size_t size, loff_t off)
{
	struct super_block *sb = fd->sb;
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_compressed_data cdata = fd->cdata;
	loff_t block;
	size_t bsize;
	ssize_t res;

	/*
	 * Request reads of all blocks before actually working with any of them.
	 * The compressed data is typically small enough that this is effective.
	 * It would be much better to make an inode for the xattr dstream and
	 * work with readahead as usual, but I'm not confident I can get that
	 * right (TODO).
	 */
	if (cdata.has_dstream && off == 0) {
		down_read(&nxi->nx_big_sem);
		apfs_nonsparse_dstream_preread(cdata.dstream);
		up_read(&nxi->nx_big_sem);
	}

	if (off >= le64_to_cpu(fd->hdr.size))
		return 0;
	if (size > le64_to_cpu(fd->hdr.size) - off)
		size = le64_to_cpu(fd->hdr.size) - off;

	block = off / APFS_COMPRESS_BLOCK;
	off -= block * APFS_COMPRESS_BLOCK;
	if (block != fd->bufblk) {
		down_read(&nxi->nx_big_sem);
		res = apfs_compress_file_read_block(fd, block);
		up_read(&nxi->nx_big_sem);
		if (res) {
			apfs_err(sb, "failed to read block into buffer");
			return res;
		}
	}
	bsize = fd->bufsize;

	if (bsize < off)
		return 0;
	bsize -= off;
	if (size > bsize)
		size = bsize;
	memcpy(buf, fd->buf + off, size);
	return size;
}

static ssize_t apfs_compress_file_read_page(struct file *filp, char *buf, loff_t off)
{
	struct apfs_compress_file_data *fd = filp->private_data;
	loff_t step;
	ssize_t block, res;
	size_t size = PAGE_SIZE;

	step = 0;
	while (step < size) {
		block = APFS_COMPRESS_BLOCK - ((off + step) & (APFS_COMPRESS_BLOCK - 1));
		if (block > size - step)
			block = size - step;
		mutex_lock(&fd->mtx);
		res = apfs_compress_file_read_from_block(fd, buf + step, block, off + step);
		mutex_unlock(&fd->mtx);
		if (res < block) {
			if (res < 0 && !step)
				return res;
			step += res > 0 ? res : 0;
			break;
		}
		step += block;
	}
	return step;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static int apfs_compress_read_folio(struct file *filp, struct folio *folio)
{
	struct page *page = &folio->page;
#else
static int apfs_compress_readpage(struct file *filp, struct page *page)
{
#endif
	char *addr = NULL;
	ssize_t ret;
	loff_t off;

	/* Mostly copied from ext4_read_inline_page() */
	off = page->index << PAGE_SHIFT;
	addr = kmap(page);
	ret = apfs_compress_file_read_page(filp, addr, off);
	flush_dcache_page(page);
	kunmap(page);
	if (ret >= 0) {
		zero_user_segment(page, ret, PAGE_SIZE);
		SetPageUptodate(page);
		ret = 0;
	}

	unlock_page(page);
	return ret;
}

const struct address_space_operations apfs_compress_aops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	.read_folio	= apfs_compress_read_folio,
#else
	.readpage	= apfs_compress_readpage,
#endif
};

/* TODO: these operations are all happening without proper locks */
const struct file_operations apfs_compress_file_operations = {
	.open		= apfs_compress_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.release	= apfs_compress_file_release,
	.mmap		= apfs_file_mmap,
};

int apfs_compress_get_size(struct inode *inode, loff_t *size)
{
	struct apfs_compress_hdr hdr;
	int res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &hdr, sizeof(hdr), 0);

	if (res < 0)
		return res;
	if (res != sizeof(hdr)) {
		apfs_err(inode->i_sb, "decmpfs header read failed");
		return 1;
	}

	if (!apfs_compress_is_supported(le32_to_cpu(hdr.algo)))
		return 1;

	*size = le64_to_cpu(hdr.size);
	return 0;
}
