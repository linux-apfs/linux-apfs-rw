// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "apfs.h"

/**
 * apfs_child_from_query - Read the child id found by a successful nonleaf query
 * @query:	the query that found the record
 * @child:	Return parameter.  The child id found.
 *
 * Reads the child id in the nonleaf node record into @child and performs a
 * basic sanity check as a protection against crafted filesystems.  Returns 0
 * on success or -EFSCORRUPTED otherwise.
 */
static int apfs_child_from_query(struct apfs_query *query, u64 *child)
{
	char *raw = query->node->object.bh->b_data;

	if (query->len != 8) /* The data on a nonleaf node is the child id */
		return -EFSCORRUPTED;

	*child = le64_to_cpup((__le64 *)(raw + query->off));
	return 0;
}

/**
 * apfs_omap_lookup_block - Find the block number of a b-tree node from its id
 * @sb:		filesystem superblock
 * @tbl:	Root of the object map to be searched
 * @id:		id of the node
 * @block:	on return, the found block number
 * @write:	get write access to the object?
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_omap_lookup_block(struct super_block *sb, struct apfs_node *tbl,
			   u64 id, u64 *block, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_key key;
	int ret = 0;

	query = apfs_alloc_query(tbl, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(id, sbi->s_xid, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto fail;

	ret = apfs_bno_from_query(query, block);
	if (ret) {
		apfs_alert(sb, "bad object map leaf block: 0x%llx",
			   query->node->object.block_nr);
		goto fail;
	}

	if (write) {
		struct apfs_omap_key key;
		struct apfs_omap_val val;
		struct buffer_head *new_bh;

		new_bh = apfs_read_object_block(sb, *block, write);
		if (IS_ERR(new_bh)) {
			ret = PTR_ERR(new_bh);
			goto fail;
		}

		key.ok_oid = cpu_to_le64(id);
		key.ok_xid = cpu_to_le64(sbi->s_xid); /* TODO: snapshots? */
		val.ov_flags = 0; /* TODO: preserve the flags */
		val.ov_size = cpu_to_le32(sb->s_blocksize);
		val.ov_paddr = cpu_to_le64(new_bh->b_blocknr);
		ret = apfs_btree_replace(query, &key, sizeof(key),
					 &val, sizeof(val));

		*block = new_bh->b_blocknr;
		brelse(new_bh);
	}

fail:
	apfs_free_query(sb, query);
	return ret;
}

/**
 * apfs_create_omap_rec - Create a record in the volume's omap tree
 * @sb:		filesystem superblock
 * @oid:	object id
 * @bno:	block number
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_create_omap_rec(struct super_block *sb, u64 oid, u64 bno)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_key key;
	struct apfs_omap_key raw_key;
	struct apfs_omap_val raw_val;
	int ret;

	query = apfs_alloc_query(sbi->s_omap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(oid, sbi->s_xid, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto fail;

	raw_key.ok_oid = cpu_to_le64(oid);
	raw_key.ok_xid = cpu_to_le64(sbi->s_xid);
	raw_val.ov_flags = 0;
	raw_val.ov_size = cpu_to_le32(sb->s_blocksize);
	raw_val.ov_paddr = cpu_to_le64(bno);

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
				&raw_val, sizeof(raw_val));

fail:
	apfs_free_query(sb, query);
	return ret;
}

/**
 * apfs_delete_omap_rec - Delete an existing record from the volume's omap tree
 * @sb:		filesystem superblock
 * @oid:	object id for the record
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_delete_omap_rec(struct super_block *sb, u64 oid)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_key key;
	int ret;

	query = apfs_alloc_query(sbi->s_omap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(oid, sbi->s_xid, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret == -ENODATA)
		ret = -EFSCORRUPTED;
	if (!ret)
		ret = apfs_btree_remove(query);

	apfs_free_query(sb, query);
	return ret;
}

/**
 * apfs_alloc_query - Allocates a query structure
 * @node:	node to be searched
 * @parent:	query for the parent node
 *
 * Callers other than apfs_btree_query() should set @parent to NULL, and @node
 * to the root of the b-tree. They should also initialize most of the query
 * fields themselves; when @parent is not NULL the query will inherit them.
 *
 * Returns the allocated query, or NULL in case of failure.
 */
struct apfs_query *apfs_alloc_query(struct apfs_node *node,
				    struct apfs_query *parent)
{
	struct apfs_query *query;

	query = kmalloc(sizeof(*query), GFP_KERNEL);
	if (!query)
		return NULL;

	/* To be released by free_query. */
	apfs_node_get(node);
	query->node = node;
	query->key = parent ? parent->key : NULL;
	query->flags = parent ?
		parent->flags & ~(APFS_QUERY_DONE | APFS_QUERY_NEXT) : 0;
	query->parent = parent;
	/* Start the search with the last record and go backwards */
	query->index = node->records;
	query->depth = parent ? parent->depth + 1 : 0;

	return query;
}

/**
 * apfs_free_query - Free a query structure
 * @sb:		filesystem superblock
 * @query:	query to free
 *
 * Also frees the ancestor queries, if they are kept.
 */
void apfs_free_query(struct super_block *sb, struct apfs_query *query)
{
	while (query) {
		struct apfs_query *parent = query->parent;

		apfs_node_put(query->node);
		kfree(query);
		query = parent;
	}
}

/**
 * apfs_btree_query - Execute a query on a b-tree
 * @sb:		filesystem superblock
 * @query:	the query to execute
 *
 * Searches the b-tree starting at @query->index in @query->node, looking for
 * the record corresponding to @query->key.
 *
 * Returns 0 in case of success and sets the @query->len, @query->off and
 * @query->index fields to the results of the query. @query->node will now
 * point to the leaf node holding the record.
 *
 * In case of failure returns an appropriate error code.
 */
int apfs_btree_query(struct super_block *sb, struct apfs_query **query)
{
	struct apfs_node *node;
	struct apfs_query *parent;
	u64 child_id;
	u32 storage = apfs_query_storage(*query);
	int err;

next_node:
	if ((*query)->depth >= 12) {
		/*
		 * We need a maximum depth for the tree so we can't loop
		 * forever if the filesystem is damaged. 12 should be more
		 * than enough to map every block.
		 */
		apfs_alert(sb, "b-tree is corrupted");
		return -EFSCORRUPTED;
	}

	err = apfs_node_query(sb, *query);
	if (err == -EAGAIN) {
		if (!(*query)->parent) /* We are at the root of the tree */
			return -ENODATA;

		/* Move back up one level and continue the query */
		parent = (*query)->parent;
		(*query)->parent = NULL; /* Don't free the parent */
		apfs_free_query(sb, *query);
		*query = parent;
		goto next_node;
	}
	if (err)
		return err;
	if (apfs_node_is_leaf((*query)->node)) /* All done */
		return 0;

	err = apfs_child_from_query(*query, &child_id);
	if (err) {
		apfs_alert(sb, "bad index block: 0x%llx",
			   (*query)->node->object.block_nr);
		return err;
	}

	/* Now go a level deeper and search the child */
	node = apfs_read_node(sb, child_id, storage, false /* write */);
	if (IS_ERR(node))
		return PTR_ERR(node);

	if (node->object.oid != child_id)
		apfs_debug(sb, "corrupt b-tree");

	/*
	 * Remember the parent node and index in case the search needs
	 * to be continued later.  TODO: allocate queries from a cache?
	 */
	*query = apfs_alloc_query(node, *query);
	apfs_node_put(node);
	goto next_node;
}

/**
 * apfs_omap_read_node - Find and read a node from a b-tree
 * @id:		id for the seeked node
 *
 * Returns NULL is case of failure, otherwise a pointer to the resulting
 * apfs_node structure.
 */
struct apfs_node *apfs_omap_read_node(struct super_block *sb, u64 id)
{
	struct apfs_node *result;

	result = apfs_read_node(sb, id, APFS_OBJ_VIRTUAL, false /* write */);
	if (IS_ERR(result))
		return result;

	if (result->object.oid != id)
		apfs_debug(sb, "corrupt b-tree");

	return result;
}

/**
 * apfs_query_join_transaction - Add the found node to the current transaction
 * @query: query that found the node
 */
int apfs_query_join_transaction(struct apfs_query *query)
{
	struct apfs_node *node = query->node;
	struct super_block *sb = node->object.sb;
	u64 oid = node->object.oid;
	u32 storage = apfs_query_storage(query);

	if (buffer_trans(node->object.bh)) /* Already in the transaction */
		return 0;
	/* Ephemeral objects are always checkpoint data */
	ASSERT(storage != APFS_OBJ_EPHEMERAL);

	node = apfs_read_node(sb, oid, storage, true /* write */);
	if (IS_ERR(node))
		return PTR_ERR(node);
	apfs_node_put(query->node);
	query->node = node;

	if (storage == APFS_OBJ_PHYSICAL && query->parent) {
		__le64 bno = cpu_to_le64(node->object.block_nr);

		/* The parent node needs to report the new location */
		return apfs_btree_replace(query->parent,
					  NULL /* key */, 0 /* key_len */,
					  &bno, sizeof(bno));
	}
	return 0;
}

/**
 * apfs_btree_change_rec_count - Update the b-tree info before a record change
 * @query:	query used to insert/remove/replace the leaf record
 * @change:	change in the record count
 * @key_len:	length of the new leaf record key (0 if removed or unchanged)
 * @val_len:	length of the new leaf record value (0 if removed or unchanged)
 *
 * Don't call this function if @query->parent was reset to NULL, or if the same
 * is true of any of its ancestor queries.
 */
static void apfs_btree_change_rec_count(struct apfs_query *query, int change,
					int key_len, int val_len)
{
	struct super_block *sb;
	struct apfs_sb_info *sbi;
	struct apfs_node *root;
	struct apfs_btree_node_phys *root_raw;
	struct apfs_btree_info *info;

	if (change == -1)
		ASSERT(!key_len && !val_len);
	ASSERT(apfs_node_is_leaf(query->node));

	while (query->parent)
		query = query->parent;
	root = query->node;
	ASSERT(apfs_node_is_root(root));

	sb = root->object.sb;
	sbi = APFS_SB(sb);
	root_raw = (void *)root->object.bh->b_data;
	info = (void *)root_raw + sb->s_blocksize - sizeof(*info);

	ASSERT(sbi->s_xid == le64_to_cpu(root_raw->btn_o.o_xid));
	if (key_len > le32_to_cpu(info->bt_longest_key))
		info->bt_longest_key = cpu_to_le32(key_len);
	if (val_len > le32_to_cpu(info->bt_longest_val))
		info->bt_longest_val = cpu_to_le32(val_len);
	le64_add_cpu(&info->bt_key_count, change);
}

/**
 * apfs_btree_change_node_count - Change the node count for a b-tree
 * @query:	query used to remove/create the node
 * @change:	change in the node count
 *
 * Also changes the node count in the volume superblock.  Don't call this
 * function if @query->parent was reset to NULL, or if the same is true of
 * any of its ancestor queries.
 */
void apfs_btree_change_node_count(struct apfs_query *query, int change)
{
	struct super_block *sb;
	struct apfs_sb_info *sbi;
	struct apfs_node *root;
	struct apfs_btree_node_phys *root_raw;
	struct apfs_btree_info *info;

	ASSERT(!apfs_node_is_leaf(query->node));

	while (query->parent)
		query = query->parent;
	root = query->node;
	ASSERT(apfs_node_is_root(root));

	sb = root->object.sb;
	sbi = APFS_SB(sb);
	root_raw = (void *)root->object.bh->b_data;
	info = (void *)root_raw + sb->s_blocksize - sizeof(*info);

	ASSERT(sbi->s_xid == le64_to_cpu(root_raw->btn_o.o_xid));
	le64_add_cpu(&info->bt_node_count, change);
}

/**
 * apfs_btree_insert - Insert a new record into a b-tree
 * @query:	query run to search for the record
 * @key:	on-disk record key
 * @key_len:	length of @key
 * @val:	on-disk record value (NULL for ghost records)
 * @val_len:	length of @val (0 for ghost records)
 *
 * The new record is placed right after the one found by @query.  On success,
 * returns 0 and sets @query to the new record; returns a negative error code
 * in case of failure.
 */
int apfs_btree_insert(struct apfs_query *query, void *key, int key_len,
		      void *val, int val_len)
{
	struct apfs_node *node = query->node;
	struct super_block *sb = node->object.sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_btree_node_phys *node_raw;
	int toc_entry_size;
	int err;

	/* Do this first, or node splits may cause @query->parent to be gone */
	if (apfs_node_is_leaf(node))
		apfs_btree_change_rec_count(query, 1 /* change */,
					    key_len, val_len);

	err = apfs_query_join_transaction(query);
	if (err)
		return err;

again:
	node = query->node;
	node_raw = (void *)node->object.bh->b_data;
	ASSERT(sbi->s_xid == le64_to_cpu(node_raw->btn_o.o_xid));

	/* TODO: support record fragmentation */
	if (node->free + key_len + val_len > node->data) {
		err = apfs_node_split(query);
		if (err)
			return err;
		goto again;
	}

	if (apfs_node_has_fixed_kv_size(node))
		toc_entry_size = sizeof(struct apfs_kvoff);
	else
		toc_entry_size = sizeof(struct apfs_kvloc);

	/* Expand the table of contents if necessary */
	if (sizeof(*node_raw) +
	    (node->records + 1) * toc_entry_size > node->key) {
		int new_key_base = node->key;
		int new_free_base = node->free;
		int inc;

		inc = APFS_BTREE_TOC_ENTRY_INCREMENT * toc_entry_size;

		new_key_base += inc;
		new_free_base += inc;
		if (new_free_base + key_len + val_len > node->data) {
			err = apfs_node_split(query);
			if (err)
				return err;
			goto again;
		}
		memmove((void *)node_raw + new_key_base,
			(void *)node_raw + node->key, node->free - node->key);

		node->key = new_key_base;
		node->free = new_free_base;
	}

	query->index++; /* The query returned the record right before @key */
	query->len = val_len;
	query->key_len = key_len;

	/* Write the record key to the end of the key area */
	query->key_off = node->free;
	memcpy((void *)node_raw + query->key_off, key, key_len);
	node->free += key_len;

	if (val) {
		/* Write the record value to the beginning of the value area */
		query->off = node->data - val_len;
		memcpy((void *)node_raw + query->off, val, val_len);
		node->data -= val_len;
	}

	/* Add the new entry to the table of contents */
	apfs_create_toc_entry(query);

	apfs_update_node(node);
	return 0;
}

/**
 * apfs_btree_remove - Remove a record from a b-tree
 * @query:	exact query that found the record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_btree_remove(struct apfs_query *query)
{
	struct apfs_node *node = query->node;
	struct super_block *sb = node->object.sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_btree_node_phys *node_raw;
	int later_entries = node->records - query->index - 1;
	int err;

	/* Do this first, or node splits may cause @query->parent to be gone */
	if (apfs_node_is_leaf(node))
		apfs_btree_change_rec_count(query, -1 /* change */,
					    0 /* key_len */, 0 /* val_len */);
	else
		apfs_btree_change_node_count(query, -1 /* change */);

	err = apfs_query_join_transaction(query);
	if (err)
		return err;

	node = query->node;
	node_raw = (void *)query->node->object.bh->b_data;
	ASSERT(sbi->s_xid == le64_to_cpu(node_raw->btn_o.o_xid));

	if (node->records == 1)
		/* Just get rid of the node.  TODO: update the node heights? */
		return apfs_delete_node(query);

	/* The first key in a node must match the parent record's */
	if (query->parent && query->index == 0) {
		int first_key_len, first_key_off;
		void *key;

		first_key_len = apfs_node_locate_key(node, 1, &first_key_off);
		if (!first_key_len)
			return -EFSCORRUPTED;
		key = (void *)node_raw + first_key_off;

		err = apfs_btree_replace(query->parent, key, first_key_len,
					 NULL /* val */, 0 /* val_len */);
		if (err)
			return err;
	}

	/* Remove the entry from the table of contents */
	if (apfs_node_has_fixed_kv_size(node)) {
		struct apfs_kvoff *toc_entry;

		toc_entry = (struct apfs_kvoff *)node_raw->btn_data +
								query->index;
		memmove(toc_entry, toc_entry + 1,
			later_entries * sizeof(*toc_entry));
	} else {
		struct apfs_kvloc *toc_entry;

		toc_entry = (struct apfs_kvloc *)node_raw->btn_data +
								query->index;
		memmove(toc_entry, toc_entry + 1,
			later_entries * sizeof(*toc_entry));
	}
	--node->records;

	/*
	 * TODO: move the edges of the key and value areas, if necessary; add
	 * the freed space to the linked list.
	 */
	node->key_free_list_len += query->key_len;
	node->val_free_list_len += query->len;

	apfs_update_node(node);
	--query->index;
	return 0;
}

/**
 * apfs_btree_replace - Replace a record in a b-tree
 * @query:	exact query that found the record
 * @key:	new on-disk record key (NULL if unchanged)
 * @key_len:	length of @key
 * @val:	new on-disk record value (NULL if unchanged)
 * @val_len:	length of @val
 *
 * It's important that the order of the records is not changed by the new @key.
 * This function is not needed to replace an old value with a new one of the
 * same length: it can just be overwritten in place.
 *
 * Returns 0 on success, and @query is left pointing to the same record; returns
 * a negative error code in case of failure.
 */
int apfs_btree_replace(struct apfs_query *query, void *key, int key_len,
		       void *val, int val_len)
{
	struct apfs_node *node = query->node;
	struct super_block *sb = node->object.sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_btree_node_phys *node_raw;
	int err;

	ASSERT(key || val);

	/* Do this first, or node splits may cause @query->parent to be gone */
	if (apfs_node_is_leaf(node))
		apfs_btree_change_rec_count(query, 0 /* change */,
					    key_len, val_len);

	err = apfs_query_join_transaction(query);
	if (err)
		return err;

again:
	node = query->node;
	node_raw = (void *)node->object.bh->b_data;
	ASSERT(sbi->s_xid == le64_to_cpu(node_raw->btn_o.o_xid));

	/* The first key in a node must match the parent record's */
	if (key && query->parent && query->index == 0) {
		err = apfs_btree_replace(query->parent, key, key_len,
					 NULL /* val */, 0 /* val_len */);
		if (err)
			return err;
	}

	/* TODO: support record fragmentation */
	if ((key_len > query->key_len || val_len > query->len) &&
	    node->free + key_len + val_len > node->data) {
		err = apfs_node_split(query);
		if (err)
			return err;
		goto again;
	}

	if (key) {
		if (key_len <= query->key_len) {
			memcpy((void *)node_raw + query->key_off, key, key_len);
			node->key_free_list_len += query->key_len - key_len;
		} else {
			query->key_off = node->free;
			memcpy((void *)node_raw + node->free, key, key_len);
			node->free += key_len;
			node->key_free_list_len += query->key_len;
		}
		query->key_len = key_len;
	}

	if (val) {
		if (val_len <= query->len) {
			memcpy((void *)node_raw + query->off, val, val_len);
			node->val_free_list_len += query->len - val_len;
		} else {
			node->data -= val_len;
			query->off = node->data;
			memcpy((void *)node_raw + node->data, val, val_len);
			node->val_free_list_len += query->len;
		}
		query->len = val_len;
	}

	/* If the key or value were resized, update the table of contents */
	if (!apfs_node_has_fixed_kv_size(node)) {
		struct apfs_kvloc *kvloc;
		int value_end;

		value_end = sb->s_blocksize;
		if (apfs_node_is_root(node))
			value_end -= sizeof(struct apfs_btree_info);

		kvloc = (struct apfs_kvloc *)node_raw->btn_data + query->index;
		kvloc->v.off = cpu_to_le16(value_end - query->off);
		kvloc->v.len = cpu_to_le16(query->len);
		kvloc->k.off = cpu_to_le16(query->key_off - node->key);
		kvloc->k.len = cpu_to_le16(query->key_len);
	}

	apfs_update_node(node);
	return 0;
}
