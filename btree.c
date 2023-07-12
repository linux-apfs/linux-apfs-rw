// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "apfs.h"

static u64 apfs_catalog_base_oid(struct apfs_query *query)
{
	struct apfs_query *root_query = NULL;

	root_query = query;
	while (root_query->parent)
		root_query = root_query->parent;

	return root_query->node->object.oid;
}

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
	struct super_block *sb = query->node->object.sb;
	char *raw = query->node->object.data;

	if (query->flags & APFS_QUERY_CAT && apfs_is_sealed(sb)) {
		struct apfs_btn_index_node_val *index_val = NULL;

		if (query->len != sizeof(*index_val)) {
			apfs_err(sb, "bad sealed index value length (%d)", query->len);
			return -EFSCORRUPTED;
		}
		index_val = (struct apfs_btn_index_node_val *)(raw + query->off);
		*child = le64_to_cpu(index_val->binv_child_oid) + apfs_catalog_base_oid(query);
	} else {
		if (query->len != 8) { /* The data on a nonleaf node is the child id */
			apfs_err(sb, "bad index value length (%d)", query->len);
			return -EFSCORRUPTED;
		}
		*child = le64_to_cpup((__le64 *)(raw + query->off));
	}
	return 0;
}

/**
 * apfs_omap_cache_lookup - Look for an oid in an omap's cache
 * @omap:	the object map
 * @oid:	object id to look up
 * @bno:	on return, the block number for the oid
 *
 * Returns 0 on success, or -1 if this mapping is not cached.
 */
static int apfs_omap_cache_lookup(struct apfs_omap *omap, u64 oid, u64 *bno)
{
	struct apfs_omap_cache *cache = &omap->omap_cache;
	struct apfs_omap_rec *record = NULL;
	int slot;
	int ret = -1;

	if (cache->disabled)
		return -1;

	/* Uninitialized cache records use OID 0, so check this just in case */
	if (!oid)
		return -1;

	slot = oid & APFS_OMAP_CACHE_SLOT_MASK;
	record = &cache->recs[slot];

	spin_lock(&cache->lock);
	if (record->oid == oid) {
		*bno = record->bno;
		ret = 0;
	}
	spin_unlock(&cache->lock);

	return ret;
}

/**
 * apfs_omap_cache_save - Save a record in an omap's cache
 * @omap:	the object map
 * @oid:	object id of the record
 * @bno:	block number for the oid
 */
static void apfs_omap_cache_save(struct apfs_omap *omap, u64 oid, u64 bno)
{
	struct apfs_omap_cache *cache = &omap->omap_cache;
	struct apfs_omap_rec *record = NULL;
	int slot;

	if (cache->disabled)
		return;

	slot = oid & APFS_OMAP_CACHE_SLOT_MASK;
	record = &cache->recs[slot];

	spin_lock(&cache->lock);
	record->oid = oid;
	record->bno = bno;
	spin_unlock(&cache->lock);
}

/**
 * apfs_omap_cache_delete - Try to delete a record from an omap's cache
 * @omap:	the object map
 * @oid:	object id of the record
 */
static void apfs_omap_cache_delete(struct apfs_omap *omap, u64 oid)
{
	struct apfs_omap_cache *cache = &omap->omap_cache;
	struct apfs_omap_rec *record = NULL;
	int slot;

	if (cache->disabled)
		return;

	slot = oid & APFS_OMAP_CACHE_SLOT_MASK;
	record = &cache->recs[slot];

	spin_lock(&cache->lock);
	if (record->oid == oid) {
		record->oid = 0;
		record->bno = 0;
	}
	spin_unlock(&cache->lock);
}

/**
 * apfs_mounted_xid - Returns the mounted xid for this superblock
 * @sb:	superblock structure
 *
 * This function is needed instead of APFS_NXI(@sb)->nx_xid in situations where
 * we might be working with a snapshot. Snapshots are read-only and should
 * mostly ignore xids, so this only appears to matter for omap lookups.
 */
static inline u64 apfs_mounted_xid(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);

	return sbi->s_snap_xid ? sbi->s_snap_xid : nxi->nx_xid;
}

/**
 * apfs_xid_in_snapshot - Check if an xid is part of a snapshot
 * @omap:	the object map
 * @xid:	the xid to check
 */
static inline bool apfs_xid_in_snapshot(struct apfs_omap *omap, u64 xid)
{
	return xid <= omap->omap_latest_snap;
}

/**
 * apfs_omap_lookup_block - Find the block number of a b-tree node from its id
 * @sb:		filesystem superblock
 * @omap:	object map to be searched
 * @id:		id of the node
 * @block:	on return, the found block number
 * @write:	get write access to the object?
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_omap_lookup_block(struct super_block *sb, struct apfs_omap *omap,
			   u64 id, u64 *block, bool write)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_query *query;
	struct apfs_key key;
	struct apfs_omap_map map = {0};
	int ret = 0;

	if (!write) {
		if (!apfs_omap_cache_lookup(omap, id, block))
			return 0;
	}

	query = apfs_alloc_query(omap->omap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(id, apfs_mounted_xid(sb), &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		if (ret != -ENODATA)
			apfs_err(sb, "query failed for oid 0x%llx, xid 0x%llx", id, apfs_mounted_xid(sb));
		goto fail;
	}

	ret = apfs_omap_map_from_query(query, &map);
	if (ret) {
		apfs_alert(sb, "bad object map leaf block: 0x%llx",
			   query->node->object.block_nr);
		goto fail;
	}
	*block = map.bno;

	if (write) {
		struct apfs_omap_key key;
		struct apfs_omap_val val;
		struct buffer_head *new_bh;
		bool preserve;

		preserve = apfs_xid_in_snapshot(omap, map.xid);

		new_bh = apfs_read_object_block(sb, *block, write, preserve);
		if (IS_ERR(new_bh)) {
			apfs_err(sb, "CoW failed for oid 0x%llx, xid 0x%llx", id, apfs_mounted_xid(sb));
			ret = PTR_ERR(new_bh);
			goto fail;
		}

		key.ok_oid = cpu_to_le64(id);
		key.ok_xid = cpu_to_le64(nxi->nx_xid);
		val.ov_flags = cpu_to_le32(map.flags);
		val.ov_size = cpu_to_le32(sb->s_blocksize);
		val.ov_paddr = cpu_to_le64(new_bh->b_blocknr);

		if (preserve)
			ret = apfs_btree_insert(query, &key, sizeof(key), &val, sizeof(val));
		else
			ret = apfs_btree_replace(query, &key, sizeof(key), &val, sizeof(val));
		if (ret)
			apfs_err(sb, "CoW omap update failed (oid 0x%llx, xid 0x%llx)", id, apfs_mounted_xid(sb));

		*block = new_bh->b_blocknr;
		brelse(new_bh);
	}

	apfs_omap_cache_save(omap, id, *block);

fail:
	apfs_free_query(query);
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_omap *omap = sbi->s_omap;
	struct apfs_query *query;
	struct apfs_key key;
	struct apfs_omap_key raw_key;
	struct apfs_omap_val raw_val;
	int ret;

	query = apfs_alloc_query(omap->omap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(oid, nxi->nx_xid, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for oid 0x%llx, bno 0x%llx", oid, bno);
		goto fail;
	}

	raw_key.ok_oid = cpu_to_le64(oid);
	raw_key.ok_xid = cpu_to_le64(nxi->nx_xid);
	raw_val.ov_flags = 0;
	raw_val.ov_size = cpu_to_le32(sb->s_blocksize);
	raw_val.ov_paddr = cpu_to_le64(bno);

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
				&raw_val, sizeof(raw_val));
	if (ret) {
		apfs_err(sb, "insertion failed for oid 0x%llx, bno 0x%llx", oid, bno);
		goto fail;
	}

	apfs_omap_cache_save(omap, oid, bno);

fail:
	apfs_free_query(query);
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_omap *omap = sbi->s_omap;
	struct apfs_query *query;
	struct apfs_key key;
	int ret;

	query = apfs_alloc_query(omap->omap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;

	apfs_init_omap_key(oid, nxi->nx_xid, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_OMAP;

	ret = apfs_btree_query(sb, &query);
	if (ret == -ENODATA) {
		apfs_err(sb, "nonexistent record (oid 0x%llx)", oid);
		ret = -EFSCORRUPTED;
		goto fail;
	}
	if (ret) {
		apfs_err(sb, "query failed (oid 0x%llx)", oid);
		goto fail;
	}
	ret = apfs_btree_remove(query);
	if (ret) {
		apfs_err(sb, "removal failed (oid 0x%llx)", oid);
		goto fail;
	}
	apfs_omap_cache_delete(omap, oid);

fail:
	apfs_free_query(query);
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
 * @query: query to free
 *
 * Also frees the ancestor queries, if they are kept.
 */
void apfs_free_query(struct apfs_query *query)
{
	while (query) {
		struct apfs_query *parent = query->parent;

		/* The caller decides whether to free the root node */
		if (query->depth != 0)
			apfs_node_free(query->node);
		kfree(query);
		query = parent;
	}
}

/**
 * apfs_query_set_before_first - Set the query to point before the first record
 * @sb:		superblock structure
 * @query:	the query to set
 *
 * Queries set in this way are used to insert a record before the first one.
 * Only the leaf gets set to the -1 entry; queries for other levels must be set
 * to 0, since the first entry in each index node will need to be modified.
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_query_set_before_first(struct super_block *sb, struct apfs_query **query)
{
	struct apfs_node *node = NULL;
	struct apfs_query *parent = NULL;
	u64 child_id;
	u32 storage = apfs_query_storage(*query);
	int err;

	while ((*query)->depth < 12) {
		if (apfs_node_is_leaf((*query)->node)) {
			(*query)->index = -1;
			return 0;
		}
		apfs_node_query_first(*query);

		err = apfs_child_from_query(*query, &child_id);
		if (err) {
			apfs_alert(sb, "bad index block: 0x%llx",
				   (*query)->node->object.block_nr);
			return err;
		}

		/* Now go a level deeper */
		node = apfs_read_node(sb, child_id, storage, false /* write */);
		if (IS_ERR(node)) {
			apfs_err(sb, "failed to read child 0x%llx of node 0x%llx", child_id, (*query)->node->object.oid);
			return PTR_ERR(node);
		}

		parent = *query;
		*query = apfs_alloc_query(node, parent);
		if (!*query) {
			apfs_node_free(node);
			*query = parent;
			return -ENOMEM;
		}
		node = NULL;
	}

	apfs_err(sb, "btree is too high");
	return -EFSCORRUPTED;
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
		apfs_err(sb, "btree is too high");
		return -EFSCORRUPTED;
	}

	err = apfs_node_query(sb, *query);
	if (err == -ENODATA && !(*query)->parent && (*query)->index == -1) {
		/*
		 * We may be trying to insert a record before all others: don't
		 * let the query give up at the root node.
		 */
		err = apfs_query_set_before_first(sb, query);
		if (err) {
			apfs_err(sb, "failed to set before the first record");
			return err;
		}
		return -ENODATA;
	} else if (err == -EAGAIN) {
		if (!(*query)->parent) /* We are at the root of the tree */
			return -ENODATA;

		/* Move back up one level and continue the query */
		parent = (*query)->parent;
		(*query)->parent = NULL; /* Don't free the parent */
		apfs_free_query(*query);
		*query = parent;
		goto next_node;
	} else if (err) {
		return err;
	}
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
	if (IS_ERR(node)) {
		apfs_err(sb, "failed to read node 0x%llx", child_id);
		return PTR_ERR(node);
	}

	if (node->object.oid != child_id)
		apfs_debug(sb, "corrupt b-tree");

	/*
	 * Remember the parent node and index in case the search needs
	 * to be continued later.
	 */
	parent = *query;
	*query = apfs_alloc_query(node, parent);
	if (!*query) {
		apfs_node_free(node);
		*query = parent;
		return -ENOMEM;
	}
	node = NULL;
	goto next_node;
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

	if (buffer_trans(node->object.o_bh)) /* Already in the transaction */
		return 0;
	/* Ephemeral objects are always checkpoint data */
	ASSERT(storage != APFS_OBJ_EPHEMERAL);
	/* Root nodes should join the transaction before the query is created */
	ASSERT(!apfs_node_is_root(node));

	node = apfs_read_node(sb, oid, storage, true /* write */);
	if (IS_ERR(node)) {
		apfs_err(sb, "Cow failed for node 0x%llx", oid);
		return PTR_ERR(node);
	}
	apfs_node_free(query->node);
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
	root_raw = (void *)root->object.data;
	info = (void *)root_raw + sb->s_blocksize - sizeof(*info);

	apfs_assert_in_transaction(sb, &root_raw->btn_o);
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
	struct apfs_node *root;
	struct apfs_btree_node_phys *root_raw;
	struct apfs_btree_info *info;

	ASSERT(!apfs_node_is_leaf(query->node));

	while (query->parent)
		query = query->parent;
	root = query->node;
	ASSERT(apfs_node_is_root(root));

	sb = root->object.sb;
	root_raw = (void *)root->object.data;
	info = (void *)root_raw + sb->s_blocksize - sizeof(*info);

	apfs_assert_in_transaction(sb, &root_raw->btn_o);
	le64_add_cpu(&info->bt_node_count, change);
}

/**
 * apfs_query_refresh - Recreate a catalog query invalidated by node splits
 * @old_query: the catalog query to refresh
 *
 * On success, @old_query is left pointing to the same leaf record, but with
 * valid ancestor queries as well. Returns a negative error code in case of
 * failure, or 0 on success.
 */
static int apfs_query_refresh(struct apfs_query *old_query)
{
	struct apfs_node *node = old_query->node;
	struct super_block *sb = node->object.sb;
	char *raw = node->object.data;
	struct apfs_query *new_query, *ancestor;
	struct apfs_key new_key;
	bool hashed = apfs_is_normalization_insensitive(sb);
	int err = 0;

	/*
	 * This function is for handling multiple splits of the same node,
	 * which are only expected when large inline xattr values are involved.
	 */
	if ((old_query->flags & APFS_QUERY_TREE_MASK) != APFS_QUERY_CAT) {
		apfs_err(sb, "non-catalog query");
		return -EFSCORRUPTED;
	}
	if (!apfs_node_is_leaf(node)) {
		apfs_err(sb, "non-leaf query");
		return -EFSCORRUPTED;
	}

	/* Build a new query that points exactly to the same key */
	err = apfs_read_cat_key(raw + old_query->key_off, old_query->key_len, &new_key, hashed);
	if (err) {
		apfs_err(sb, "failed to read the key");
		return err;
	}
	new_query = apfs_alloc_query(APFS_SB(sb)->s_cat_root, NULL /* parent */);
	if (!new_query)
		return -ENOMEM;
	new_query->key = &new_key;
	new_query->flags = APFS_QUERY_CAT | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &new_query);
	if (err) {
		apfs_err(sb, "failed to rerun");
		goto fail;
	}

	/* Set the original query flags and key on the new query */
	for (ancestor = new_query; ancestor; ancestor = ancestor->parent) {
		ancestor->flags = old_query->flags;
		ancestor->key = old_query->key;
	}

	/* Replace the parent of the original query with the new valid one */
	apfs_free_query(old_query->parent);
	old_query->parent = new_query->parent;
	new_query->parent = NULL;

fail:
	apfs_free_query(new_query);
	return err;
}

/**
 * apfs_query_is_orphan - Check if all of a query's ancestors are set
 * @query: the query to check
 *
 * A query may lose some of its ancestors during a node split. This can be
 * used to check if that has happened.
 *
 * TODO: running this check early on the insert, remove and replace functions
 * could be used to simplify several callers that do their own query refresh.
 */
static bool apfs_query_is_orphan(const struct apfs_query *query)
{
	while (query) {
		if (apfs_node_is_root(query->node))
			return false;
		query = query->parent;
	}
	return true;
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
	struct apfs_btree_node_phys *node_raw;
	int err;

	/* Do this first, or node splits may cause @query->parent to be gone */
	if (apfs_node_is_leaf(node))
		apfs_btree_change_rec_count(query, 1 /* change */,
					    key_len, val_len);

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}

again:
	node = query->node;
	node_raw = (void *)node->object.data;
	apfs_assert_in_transaction(node->object.sb, &node_raw->btn_o);

	err = apfs_node_insert(query, key, key_len, val, val_len);
	if (err == -ENOSPC) {
		if (!query->parent && !apfs_node_is_root(node)) {
			err = apfs_query_refresh(query);
			if (err) {
				apfs_err(sb, "query refresh failed");
				return err;
			}
			if (node->records == 1) {
				/* The new record just won't fit in the node */
				return apfs_create_single_rec_node(query, key, key_len, val, val_len);
			}
		}
		err = apfs_node_split(query);
		if (err) {
			apfs_err(sb, "node split failed");
			return err;
		}
		goto again;
	} else if (err) {
		apfs_err(sb, "node record insertion failed");
		return err;
	}

	/* This can only happen when we insert a record before all others */
	if (query->parent && query->index == 0) {
		err = apfs_btree_replace(query->parent, key, key_len,
					 NULL /* val */, 0 /* val_len */);
		if (err)
			apfs_err(sb, "parent update failed");
	}
	return err;
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
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}

	node = query->node;
	node_raw = (void *)query->node->object.data;
	apfs_assert_in_transaction(node->object.sb, &node_raw->btn_o);

	if (node->records == 1) {
		if (query->parent) {
			/* Just get rid of the node */
			return apfs_delete_node(query);
		} else {
			/* All descendants are gone, root is the whole tree */
			node_raw->btn_level = 0;
			node->flags |= APFS_BTNODE_LEAF;
		}
	}

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
		if (err) {
			apfs_err(sb, "parent update failed");
			return err;
		}
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

	apfs_node_free_range(node, query->key_off, query->key_len);
	apfs_node_free_range(node, query->off, query->len);

	--node->records;
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
	struct apfs_btree_node_phys *node_raw;
	int err;

	ASSERT(key || val);

	/* Do this first, or node splits may cause @query->parent to be gone */
	if (apfs_node_is_leaf(node)) {
		if (apfs_query_is_orphan(query)) {
			err = apfs_query_refresh(query);
			if (err)
				return err;
		}
		apfs_btree_change_rec_count(query, 0 /* change */,
					    key_len, val_len);
	}

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}

again:
	node = query->node;
	node_raw = (void *)node->object.data;
	apfs_assert_in_transaction(sb, &node_raw->btn_o);

	/* The first key in a node must match the parent record's */
	if (key && query->parent && query->index == 0) {
		err = apfs_btree_replace(query->parent, key, key_len,
					 NULL /* val */, 0 /* val_len */);
		if (err) {
			apfs_err(sb, "parent update failed");
			return err;
		}
	}

	err = apfs_node_replace(query, key, key_len, val, val_len);
	if (err == -ENOSPC) {
		if (!query->parent && !apfs_node_is_root(node)) {
			if (node->records == 1) {
				/* Node is defragmented, ENOSPC is absurd */
				WARN_ON(1);
				return -EFSCORRUPTED;
			}
			err = apfs_query_refresh(query);
			if (err) {
				apfs_err(sb, "query refresh failed");
				return err;
			}
		}
		err = apfs_node_split(query);
		if (err) {
			apfs_err(sb, "node split failed");
			return err;
		}
		goto again;
	}
	return err;
}
