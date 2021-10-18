// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include "apfs.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
typedef int vm_fault_t;
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
	struct inode *inode = file_inode(vma->vm_file);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh, *head;
	vm_fault_t ret = VM_FAULT_LOCKED;
	struct apfs_max_ops maxops;
	int blkcount = PAGE_SIZE >> inode->i_blkbits;
	unsigned int blocksize, block_start, len;
	u64 size;
	int err = 0;

	sb_start_pagefault(inode->i_sb);
	file_update_time(vma->vm_file);

	/* Placeholder values, I need to get back to this in the future */
	maxops.cat = APFS_UPDATE_INODE_MAXOPS() +
		     blkcount * APFS_GET_NEW_BLOCK_MAXOPS();
	maxops.blks = blkcount;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		goto out;
	apfs_inode_join_transaction(sb, inode);

	lock_page(page);
	wait_for_stable_page(page);
	if (page->mapping != inode->i_mapping) {
		ret = VM_FAULT_NOPAGE;
		goto out_unlock;
	}

	if (!page_has_buffers(page))
		create_empty_buffers(page, sb->s_blocksize, 0);

	size = i_size_read(inode);
	if (page->index == size >> PAGE_SHIFT)
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
	if (err)
		goto out_abort;
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
		ret = block_page_mkwrite_return(err);
	sb_end_pagefault(inode->i_sb);
	return ret;
}

static const struct vm_operations_struct apfs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= apfs_page_mkwrite,
};

static int apfs_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
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

const struct file_operations apfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= apfs_file_mmap,
	.open		= generic_file_open,
	.fsync		= apfs_fsync,
	.unlocked_ioctl	= apfs_file_ioctl,
};

const struct inode_operations apfs_file_inode_operations = {
	.getattr	= apfs_getattr,
	.listxattr	= apfs_listxattr,
	.setattr	= apfs_setattr,
	.update_time	= apfs_update_time,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	.fileattr_get	= apfs_fileattr_get,
	.fileattr_set	= apfs_fileattr_set,
#endif
};
