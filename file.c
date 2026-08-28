// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include "apfs.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#include <linux/splice.h>
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
typedef int vm_fault_t;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
static void wait_for_stable_page(struct page *page)
{
	return folio_wait_stable(page_folio(page));
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
static vm_fault_t apfs_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
{
#else
static vm_fault_t apfs_page_mkwrite(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#endif
	struct page *page = vmf->page;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	struct folio *folio;
#endif
	struct inode *inode = file_inode(vma->vm_file);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh, *head;
	vm_fault_t ret = VM_FAULT_LOCKED;
	unsigned int blocksize, block_start, len;
	u64 size;
	int err = 0;

	sb_start_pagefault(inode->i_sb);
	file_update_time(vma->vm_file);

	err = apfs_transaction_start(sb, APFS_TRANS_REG);
	if (err)
		goto out;
	apfs_inode_join_transaction(sb, inode);

	err = apfs_inode_create_exclusive_dstream(inode);
	if (err) {
		apfs_err(sb, "dstream creation failed for ino 0x%llx", apfs_ino(inode));
		goto out_abort;
	}

	lock_page(page);
	wait_for_stable_page(page);
	if (page->mapping != inode->i_mapping) {
		ret = VM_FAULT_NOPAGE;
		goto out_unlock;
	}

	if (!page_has_buffers(page)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
		create_empty_buffers(page, sb->s_blocksize, 0);
#else
		folio = page_folio(page);
		bh = folio_buffers(folio);
		if (!bh)
			bh = create_empty_buffers(folio, sb->s_blocksize, 0);
#endif
	}

	size = i_size_read(inode);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	if (page_folio(page)->index == size >> PAGE_SHIFT)
#else
	if (page->index == size >> PAGE_SHIFT)
#endif
		len = size & ~PAGE_MASK;
	else
		len = PAGE_SIZE;

	/* The blocks were read on the fault, mark them as unmapped for CoW */
	head = page_buffers(page);
	blocksize = head->b_size;
	for (bh = head, block_start = 0; bh != head || !block_start;
	     block_start += blocksize, bh = bh->b_this_page) {
		if (len > block_start) {
			/* If it's not a hole, the fault read it already */
			ASSERT(!buffer_mapped(bh) || buffer_uptodate(bh));
			if (buffer_trans(bh))
				continue;
			clear_buffer_mapped(bh);
		}
	}
	unlock_page(page); /* XXX: race? */

	err = block_page_mkwrite(vma, vmf, apfs_get_new_block);
	if (err) {
		apfs_err(sb, "mkwrite failed for ino 0x%llx", apfs_ino(inode));
		goto out_abort;
	}
	set_page_dirty(page);

	/* An immediate commit would leave the page unlocked */
	APFS_SB(sb)->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_DEFER_COMMIT;

	err = apfs_transaction_commit(sb);
	if (err)
		goto out_unlock;
	goto out;

out_unlock:
	unlock_page(page);
out_abort:
	apfs_transaction_abort(sb);
out:
	if (err)
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0) && !RHEL_VERSION_GE(9, 5)
		ret = block_page_mkwrite_return(err);
#else
		ret = vmf_fs_error(err);
#endif
	sb_end_pagefault(inode->i_sb);
	return ret;
}

static const struct vm_operations_struct apfs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= apfs_page_mkwrite,
};

int apfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0) || RHEL_VERSION_GE(9, 3)
	if (!mapping->a_ops->read_folio)
#else
	if (!mapping->a_ops->readpage)
#endif
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &apfs_file_vm_ops;
	return 0;
}

/*
 * Just flush the whole transaction for now (TODO), since that's technically
 * correct and easy to implement.
 */
int apfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct super_block *sb = inode->i_sb;

	return apfs_sync_fs(sb, true /* wait */);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
static ssize_t apfs_copy_file_range(struct file *src_file, loff_t src_off,
				    struct file *dst_file, loff_t dst_off,
				    size_t len, unsigned int flags)
{
	return (splice_copy_file_range(src_file, src_off,
		dst_file, dst_off, len));
}
#endif

const struct file_operations apfs_file_operations = {
	.llseek			= generic_file_llseek,
	.read_iter		= generic_file_read_iter,
	.write_iter		= generic_file_write_iter,
	.mmap			= apfs_file_mmap,
	.open			= generic_file_open,
	.fsync			= apfs_fsync,
	.unlocked_ioctl		= apfs_file_ioctl,

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	.copy_file_range	= apfs_copy_file_range,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	.copy_file_range	= generic_copy_file_range,
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	.remap_file_range	= apfs_remap_file_range,
#else
	.clone_file_range	= apfs_clone_file_range,
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
	.splice_read		= generic_file_splice_read,
#else
	.splice_read		= filemap_splice_read,
#endif
	.splice_write		= iter_file_splice_write,
};

/*
 * This is only needed to test clones with xfstests, so we only support the
 * kernel versions I actually run tests on these days.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)

/* generic_block_fiemap() was removed at this point, so we keep a copy */
# if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

#include <linux/fiemap.h>

static inline sector_t logical_to_blk(struct inode *inode, loff_t offset)
{
	return (offset >> inode->i_blkbits);
}

static inline loff_t blk_to_logical(struct inode *inode, sector_t blk)
{
	return (blk << inode->i_blkbits);
}

static int __generic_block_fiemap(struct inode *inode,
			   struct fiemap_extent_info *fieinfo, loff_t start,
			   loff_t len, get_block_t *get_block)
{
	struct buffer_head map_bh;
	sector_t start_blk, last_blk;
	loff_t isize = i_size_read(inode);
	u64 logical = 0, phys = 0, size = 0;
	u32 flags = FIEMAP_EXTENT_MERGED;
	bool past_eof = false, whole_file = false;
	int ret = 0;

	ret = fiemap_prep(inode, fieinfo, start, &len, FIEMAP_FLAG_SYNC);
	if (ret)
		return ret;

	/*
	 * Either the i_mutex or other appropriate locking needs to be held
	 * since we expect isize to not change at all through the duration of
	 * this call.
	 */
	if (len >= isize) {
		whole_file = true;
		len = isize;
	}

	/*
	 * Some filesystems can't deal with being asked to map less than
	 * blocksize, so make sure our len is at least block length.
	 */
	if (logical_to_blk(inode, len) == 0)
		len = blk_to_logical(inode, 1);

	start_blk = logical_to_blk(inode, start);
	last_blk = logical_to_blk(inode, start + len - 1);

	do {
		/*
		 * we set b_size to the total size we want so it will map as
		 * many contiguous blocks as possible at once
		 */
		memset(&map_bh, 0, sizeof(struct buffer_head));
		map_bh.b_size = len;

		ret = get_block(inode, start_blk, &map_bh, 0);
		if (ret)
			break;

		/* HOLE */
		if (!buffer_mapped(&map_bh)) {
			start_blk++;

			/*
			 * We want to handle the case where there is an
			 * allocated block at the front of the file, and then
			 * nothing but holes up to the end of the file properly,
			 * to make sure that extent at the front gets properly
			 * marked with FIEMAP_EXTENT_LAST
			 */
			if (!past_eof &&
			    blk_to_logical(inode, start_blk) >= isize)
				past_eof = 1;

			/*
			 * First hole after going past the EOF, this is our
			 * last extent
			 */
			if (past_eof && size) {
				flags = FIEMAP_EXTENT_MERGED|FIEMAP_EXTENT_LAST;
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size,
							      flags);
			} else if (size) {
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size, flags);
				size = 0;
			}

			/* if we have holes up to/past EOF then we're done */
			if (start_blk > last_blk || past_eof || ret)
				break;
		} else {
			/*
			 * We have gone over the length of what we wanted to
			 * map, and it wasn't the entire file, so add the extent
			 * we got last time and exit.
			 *
			 * This is for the case where say we want to map all the
			 * way up to the second to the last block in a file, but
			 * the last block is a hole, making the second to last
			 * block FIEMAP_EXTENT_LAST.  In this case we want to
			 * see if there is a hole after the second to last block
			 * so we can mark it properly.  If we found data after
			 * we exceeded the length we were requesting, then we
			 * are good to go, just add the extent to the fieinfo
			 * and break
			 */
			if (start_blk > last_blk && !whole_file) {
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size,
							      flags);
				break;
			}

			/*
			 * if size != 0 then we know we already have an extent
			 * to add, so add it.
			 */
			if (size) {
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size,
							      flags);
				if (ret)
					break;
			}

			logical = blk_to_logical(inode, start_blk);
			phys = blk_to_logical(inode, map_bh.b_blocknr);
			size = map_bh.b_size;
			flags = FIEMAP_EXTENT_MERGED;

			start_blk += logical_to_blk(inode, size);

			/*
			 * If we are past the EOF, then we need to make sure as
			 * soon as we find a hole that the last extent we found
			 * is marked with FIEMAP_EXTENT_LAST
			 */
			if (!past_eof && logical + size >= isize)
				past_eof = true;
		}
		cond_resched();
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

	} while (1);

	/* If ret is 1 then we just hit the end of the extent array */
	if (ret == 1)
		ret = 0;

	return ret;
}

static int generic_block_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo, u64 start,
				u64 len, get_block_t *get_block)
{
	int ret;
	inode_lock(inode);
	ret = __generic_block_fiemap(inode, fieinfo, start, len, get_block);
	inode_unlock(inode);
	return ret;
}

# endif

static int apfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo, u64 start, u64 len)
{
	return generic_block_fiemap(inode, fieinfo, start, len, apfs_get_block);
}

#endif

const struct inode_operations apfs_file_inode_operations = {
	.getattr	= apfs_getattr,
	.listxattr	= apfs_listxattr,
	.setattr	= apfs_setattr,
	.update_time	= apfs_update_time,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	.fileattr_get	= apfs_fileattr_get,
	.fileattr_set	= apfs_fileattr_set,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	.fiemap		= apfs_fiemap,
#endif
};
