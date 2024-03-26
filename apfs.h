/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#ifndef _APFS_H
#define _APFS_H

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/version.h>
#include "apfs_raw.h"

#define EFSBADCRC	EBADMSG		/* Bad CRC detected */
#define EFSCORRUPTED	EUCLEAN		/* Filesystem is corrupted */

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0) /* SB_RDONLY came in 4.14 */
#define SB_RDONLY	MS_RDONLY
#define SB_SILENT	MS_SILENT
#define SB_NOSEC	MS_NOSEC
#define SB_ACTIVE	MS_ACTIVE
static inline bool sb_rdonly(const struct super_block *sb) { return sb->s_flags & SB_RDONLY; }
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0)
#define lockdep_assert_held_write(l)	((void)(l))
#endif

/* Compatibility wrapper around submit_bh() */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
#define apfs_submit_bh(op, op_flags, bh) submit_bh(op, op_flags, bh)
#else
#define apfs_submit_bh(op, op_flags, bh) submit_bh(op | op_flags, bh)
#endif

/*
 * Parameter for the snapshot creation ioctl
 */
struct apfs_ioctl_snap_name {
	char name[APFS_SNAP_MAX_NAMELEN + 1];
};

#define APFS_IOC_SET_DFLT_PFK	_IOW('@', 0x80, struct apfs_wrapped_crypto_state)
#define APFS_IOC_SET_DIR_CLASS	_IOW('@', 0x81, u32)
#define APFS_IOC_SET_PFK	_IOW('@', 0x82, struct apfs_wrapped_crypto_state)
#define APFS_IOC_GET_CLASS	_IOR('@', 0x83, u32)
#define APFS_IOC_GET_PFK	_IOR('@', 0x84, struct apfs_wrapped_crypto_state)
#define APFS_IOC_TAKE_SNAPSHOT	_IOW('@', 0x85, struct apfs_ioctl_snap_name)

/*
 * In-memory representation of an APFS object
 */
struct apfs_object {
	struct super_block *sb;
	u64 block_nr;
	u64 oid;

	/*
	 * Buffer head containing the one block of the object, may be NULL if
	 * the object is only in memory. TODO: support objects with more than
	 * one block.
	 */
	struct buffer_head *o_bh;
	char *data; /* The raw object */
	bool ephemeral; /* Is this an ephemeral object? */
};

/* Constants used in managing the size of a node's table of contents */
#define APFS_BTREE_TOC_ENTRY_INCREMENT	8
#define APFS_BTREE_TOC_ENTRY_MAX_UNUSED	(2 * BTREE_TOC_ENTRY_INCREMENT)

/*
 * In-memory representation of an APFS node
 */
struct apfs_node {
	u32 tree_type;		/* Tree type (subtype of the node object) */
	u16 flags;		/* Node flags */
	u32 records;		/* Number of records in the node */

	int key;		/* Offset of the key area in the block */
	int free;		/* Offset of the free area in the block */
	int data;		/* Offset of the data area in the block */

	int key_free_list_len;	/* Length of the fragmented free key space */
	int val_free_list_len;	/* Length of the fragmented free value space */

	struct apfs_object object; /* Object holding the node */
};

/**
 * apfs_node_is_leaf - Check if a b-tree node is a leaf
 * @node: the node to check
 */
static inline bool apfs_node_is_leaf(struct apfs_node *node)
{
	return (node->flags & APFS_BTNODE_LEAF) != 0;
}

/**
 * apfs_node_is_root - Check if a b-tree node is the root
 * @node: the node to check
 */
static inline bool apfs_node_is_root(struct apfs_node *node)
{
	return (node->flags & APFS_BTNODE_ROOT) != 0;
}

/**
 * apfs_node_has_fixed_kv_size - Check if a b-tree node has fixed key/value
 * sizes
 * @node: the node to check
 */
static inline bool apfs_node_has_fixed_kv_size(struct apfs_node *node)
{
	return (node->flags & APFS_BTNODE_FIXED_KV_SIZE) != 0;
}

/*
 * Space manager data in memory.
 */
struct apfs_spaceman {
	struct apfs_spaceman_phys *sm_raw; /* On-disk spaceman structure */
	struct apfs_nxsb_info	  *sm_nxi; /* Container superblock */
	u32			  sm_size; /* Size of @sm_raw in bytes */

	u32 sm_blocks_per_chunk;	/* Blocks covered by a bitmap block */
	u32 sm_chunks_per_cib;		/* Chunk count in a chunk-info block */
	u64 sm_block_count;		/* Block count for the container */
	u64 sm_chunk_count;		/* Number of bitmap blocks */
	u32 sm_cib_count;		/* Number of chunk-info blocks */
	u64 sm_free_count;		/* Number of free blocks */
	u32 sm_addr_offset;		/* Offset of cib addresses in @sm_raw */

	/*
	 * A range of freed blocks not yet put in the free queue. Extend this as
	 * much as possible before creating an actual record.
	 */
	u64 sm_free_cache_base;
	u64 sm_free_cache_blkcnt;

	/* Shift to match an ip block with its bitmap in the array */
	int sm_ip_bmaps_shift;
	/* Mask to find an ip block's offset inside its ip bitmap */
	u32 sm_ip_bmaps_mask;
	/* Number of ip bitmaps */
	u32 sm_ip_bmaps_count;
	/* List of ip bitmaps, in order */
	struct buffer_head *sm_ip_bmaps[];
};

#define TRANSACTION_MAIN_QUEUE_MAX	4096
#define TRANSACTION_BUFFERS_MAX		65536
#define TRANSACTION_STARTS_MAX		65536

/* Possible states for the container transaction structure */
#define APFS_NX_TRANS_FORCE_COMMIT	1	/* Commit guaranteed */
#define APFS_NX_TRANS_DEFER_COMMIT	2	/* Commit banned right now */
#define APFS_NX_TRANS_COMMITTING	4	/* Commit ongoing */
#define APFS_NX_TRANS_INCOMPLETE_BLOCK	8	/* A data block is not written in full */

/*
 * Structure that keeps track of a container transaction.
 */
struct apfs_nx_transaction {
	unsigned int t_state;

	struct list_head t_inodes;	/* List of inodes in the transaction */
	struct list_head t_buffers;	/* List of buffers in the transaction */
	size_t t_buffers_count;		/* Count of items on the list */
	int t_starts_count;		/* Count of starts for transaction */
};

/*
 * Structure that keeps track of a volume transaction.
 */
struct apfs_vol_transaction {
	struct buffer_head *t_old_vsb;  /* Volume superblock being replaced */

	struct apfs_node t_old_omap_root; /* Omap root node being replaced */
	struct apfs_node t_old_cat_root;  /* Catalog root node being replaced */
};

/* State bits for buffer heads in a transaction */
#define BH_TRANS	BH_PrivateStart		/* Attached to a transaction */
#define BH_CSUM		(BH_PrivateStart + 1)	/* Requires checksum update */
BUFFER_FNS(TRANS, trans);
BUFFER_FNS(CSUM, csum);

/*
 * Additional information for a buffer in a transaction.
 */
struct apfs_bh_info {
	struct buffer_head	*bh;	/* The buffer head */
	struct list_head	list;	/* List of buffers in the transaction */
};

/*
 * Used to report how many operations may be needed for a transaction
 */
struct apfs_max_ops {
	int cat;	/* Maximum catalog records that may need changing */
	int blks;	/* Maximum extent blocks that may need changing */
};

/*
 * List entry for an in-memory ephemeral object
 */
struct apfs_ephemeral_object_info {
	u64	oid;		/* Ephemeral object id */
	u32	size;		/* Size of the object in bytes */
	void	*object;	/* In-memory address of the object */
};

/*
 * We allocate a fixed space for the list of ephemeral objects. I don't
 * actually know how big this should be allowed to get, but all the objects
 * must be written down with each transaction commit, so probably not too big.
 */
#define APFS_EPHEMERAL_LIST_SIZE	32768
#define APFS_EPHEMERAL_LIST_LIMIT	(APFS_EPHEMERAL_LIST_SIZE / sizeof(struct apfs_ephemeral_object_info))

/* Mount option flags for a container */
#define APFS_CHECK_NODES	1
#define APFS_READWRITE		2

/*
 * Container superblock data in memory
 */
struct apfs_nxsb_info {
	struct block_device *nx_bdev; /* Device for the container */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	struct file *nx_bdev_file;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	struct bdev_handle *nx_bdev_handle;
#endif
	struct apfs_nx_superblock *nx_raw; /* On-disk main sb */
	u64 nx_bno; /* Current block number for the checkpoint superblock */
	u64 nx_xid; /* Latest transaction id */

	/* List of ephemeral objects in memory (except the superblock) */
	struct apfs_ephemeral_object_info *nx_eph_list;
	int nx_eph_count;

	struct list_head vol_list;	/* List of mounted volumes in container */

	unsigned int nx_flags;	/* Mount options shared by all volumes */
	unsigned int nx_refcnt; /* Number of mounted volumes in container */

	/* TODO: handle block sizes above the maximum of PAGE_SIZE? */
	unsigned long nx_blocksize;
	unsigned char nx_blocksize_bits;

	struct apfs_spaceman *nx_spaceman;
	struct apfs_nx_transaction nx_transaction;

	/* For now, a single semaphore for every operation */
	struct rw_semaphore nx_big_sem;

	/* List of currently mounted containers */
	struct list_head nx_list;
};

extern struct mutex nxs_mutex;

/*
 * Omap mapping in memory.
 * TODO: could this and apfs_omap_rec be the same struct?
 */
struct apfs_omap_map {
	u64 xid;
	u64 bno;
	u32 flags;
};

/*
 * Omap record data in memory
 */
struct apfs_omap_rec {
	u64 oid;
	u64 bno;
};

#define APFS_OMAP_CACHE_SLOTS		128
#define APFS_OMAP_CACHE_SLOT_MASK	(APFS_OMAP_CACHE_SLOTS - 1)

/**
 * Cache of omap records
 */
struct apfs_omap_cache {
	struct apfs_omap_rec recs[APFS_OMAP_CACHE_SLOTS];
	bool disabled;
	spinlock_t lock;
};

/*
 * Omap structure shared by all snapshots for the same volume.
 */
struct apfs_omap {
	struct apfs_node *omap_root;
	struct apfs_omap_cache omap_cache;

	/* Transaction id for most recent snapshot */
	u64 omap_latest_snap;

	/* Number of snapshots sharing this omap */
	unsigned int omap_refcnt;
};

/*
 * Volume superblock data in memory
 */
struct apfs_sb_info {
	struct apfs_nxsb_info *s_nxi; /* In-memory container sb for volume */
	struct list_head list;		/* List of mounted volumes in container */
	struct apfs_superblock *s_vsb_raw; /* On-disk volume sb */

	dev_t s_anon_dev; /* Anonymous device for this volume-snapshot */

	char *s_snap_name; /* Label for the mounted snapshot */
	u64 s_snap_xid; /* Transaction id for mounted snapshot */

	struct apfs_node *s_cat_root;	/* Root of the catalog tree */
	struct apfs_omap *s_omap;	/* The object map */

	struct apfs_object s_vobject;	/* Volume superblock object */

	/* Mount options */
	unsigned int s_vol_nr;		/* Index of the volume in the sb list */
	kuid_t s_uid;			/* uid to override on-disk uid */
	kgid_t s_gid;			/* gid to override on-disk gid */

	struct apfs_crypto_state_val *s_dflt_pfk; /* default per-file key */

	struct apfs_vol_transaction s_transaction;
	int s_trans_buffers_max;

	struct inode *s_private_dir;	/* Inode for the private directory */
	struct work_struct s_orphan_cleanup_work;
};

static inline struct apfs_sb_info *APFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline bool apfs_is_sealed(struct super_block *sb)
{
	u64 flags = le64_to_cpu(APFS_SB(sb)->s_vsb_raw->apfs_incompatible_features);

	return flags & APFS_INCOMPAT_SEALED_VOLUME;
}

/**
 * apfs_vol_is_encrypted - Check if a volume is encrypting files
 * @sb: superblock
 */
static inline bool apfs_vol_is_encrypted(struct super_block *sb)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;

	return (vsb_raw->apfs_fs_flags & cpu_to_le64(APFS_FS_UNENCRYPTED)) == 0;
}

/**
 * APFS_NXI - Get the shared container info for a volume's superblock
 * @sb: superblock structure
 */
static inline struct apfs_nxsb_info *APFS_NXI(struct super_block *sb)
{
	return APFS_SB(sb)->s_nxi;
}

/**
 * APFS_SM - Get the shared spaceman struct for a volume's superblock
 * @sb: superblock structure
 */
static inline struct apfs_spaceman *APFS_SM(struct super_block *sb)
{
	return APFS_NXI(sb)->nx_spaceman;
}

static inline bool apfs_is_case_insensitive(struct super_block *sb)
{
	return (APFS_SB(sb)->s_vsb_raw->apfs_incompatible_features &
	       cpu_to_le64(APFS_INCOMPAT_CASE_INSENSITIVE)) != 0;
}

static inline bool apfs_is_normalization_insensitive(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	u64 flags = le64_to_cpu(sbi->s_vsb_raw->apfs_incompatible_features);

	if (apfs_is_case_insensitive(sb))
		return true;
	if (flags & APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)
		return true;
	return false;
}

/**
 * apfs_max_maps_per_block - Find the maximum map count for a mapping block
 * @sb: superblock structure
 */
static inline int apfs_max_maps_per_block(struct super_block *sb)
{
	unsigned long maps_size;

	maps_size = (sb->s_blocksize - sizeof(struct apfs_checkpoint_map_phys));
	return maps_size / sizeof(struct apfs_checkpoint_mapping);
}

/*
 * In-memory representation of a key, as relevant for a b-tree query.
 */
struct apfs_key {
	u64		id;
	u64		number;	/* Extent offset, name hash or transaction id */
	const char	*name;	/* On-disk name string */
	u8		type;	/* Record type (0 for the omap) */
};

/**
 * apfs_init_free_queue_key - Initialize an in-memory key for a free queue query
 * @xid:	transaction id
 * @paddr:	block number
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_free_queue_key(u64 xid, u64 paddr,
					    struct apfs_key *key)
{
	key->id = xid;
	key->type = 0;
	key->number = paddr;
	key->name = NULL;
}

/**
 * apfs_init_omap_key - Initialize an in-memory key for an omap query
 * @oid:	object id
 * @xid:	latest transaction id
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_omap_key(u64 oid, u64 xid, struct apfs_key *key)
{
	key->id = oid;
	key->type = 0;
	key->number = xid;
	key->name = NULL;
}

/**
 * apfs_init_extent_key - Initialize an in-memory key for an extentref query
 * @bno:	physical block number for the start of the extent
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_extent_key(u64 bno, struct apfs_key *key)
{
	key->id = bno;
	key->type = APFS_TYPE_EXTENT;
	key->number = 0;
	key->name = NULL;
}

/**
 * apfs_init_inode_key - Initialize an in-memory key for an inode query
 * @ino:	inode number
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_inode_key(u64 ino, struct apfs_key *key)
{
	key->id = ino;
	key->type = APFS_TYPE_INODE;
	key->number = 0;
	key->name = NULL;
}

/**
 * apfs_init_file_extent_key - Initialize an in-memory key for an extent query
 * @id:		extent id
 * @offset:	logical address (0 for a multiple query)
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_file_extent_key(u64 id, u64 offset,
					     struct apfs_key *key)
{
	key->id = id;
	key->type = APFS_TYPE_FILE_EXTENT;
	key->number = offset;
	key->name = NULL;
}

static inline void apfs_init_fext_key(u64 id, u64 offset, struct apfs_key *key)
{
	key->id = id;
	key->type = 0;
	key->number = offset;
	key->name = NULL;
}

/**
 * apfs_init_dstream_id_key - Initialize an in-memory key for a dstream query
 * @id:		data stream id
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_dstream_id_key(u64 id, struct apfs_key *key)
{
	key->id = id;
	key->type = APFS_TYPE_DSTREAM_ID;
	key->number = 0;
	key->name = NULL;
}

/**
 * apfs_init_crypto_state_key - Initialize an in-memory key for a crypto query
 * @id:		crypto state id
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_crypto_state_key(u64 id, struct apfs_key *key)
{
	key->id = id;
	key->type = APFS_TYPE_CRYPTO_STATE;
	key->number = 0;
	key->name = NULL;
}

/**
 * apfs_init_sibling_link_key - Initialize an in-memory key for a sibling query
 * @ino:	inode number
 * @id:		sibling id
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_sibling_link_key(u64 ino, u64 id,
					      struct apfs_key *key)
{
	key->id = ino;
	key->type = APFS_TYPE_SIBLING_LINK;
	key->number = id; /* Only guessing (TODO) */
	key->name = NULL;
}

/**
 * apfs_init_sibling_map_key - Initialize in-memory key for a sibling map query
 * @id:		sibling id
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_sibling_map_key(u64 id, struct apfs_key *key)
{
	key->id = id;
	key->type = APFS_TYPE_SIBLING_MAP;
	key->number = 0;
	key->name = NULL;
}

extern void apfs_init_drec_key(struct super_block *sb, u64 ino, const char *name,
			       unsigned int name_len, struct apfs_key *key);

/**
 * apfs_init_xattr_key - Initialize an in-memory key for a xattr query
 * @ino:	inode number of the parent file
 * @name:	xattr name (NULL for a multiple query)
 * @key:	apfs_key structure to initialize
 */
static inline void apfs_init_xattr_key(u64 ino, const char *name,
				       struct apfs_key *key)
{
	key->id = ino;
	key->type = APFS_TYPE_XATTR;
	key->number = 0;
	key->name = name;
}

static inline void apfs_init_snap_metadata_key(u64 xid, struct apfs_key *key)
{
	key->id = xid;
	key->type = APFS_TYPE_SNAP_METADATA;
	key->number = 0;
	key->name = NULL;
}

static inline void apfs_init_snap_name_key(const char *name, struct apfs_key *key)
{
	key->id = APFS_SNAP_NAME_OBJ_ID;
	key->type = APFS_TYPE_SNAP_NAME;
	key->number = 0;
	key->name = name;
}

static inline void apfs_init_omap_snap_key(u64 xid, struct apfs_key *key)
{
	key->id = xid;
	key->type = 0;
	key->number = 0;
	key->name = NULL;
}

/**
 * apfs_key_set_hdr - Set the header for a raw catalog key
 * @type:	record type
 * @id:		record id
 * @key:	the key to initialize
 */
static inline void apfs_key_set_hdr(u64 type, u64 id, void *key)
{
	struct apfs_key_header *hdr = key;

	hdr->obj_id_and_type = cpu_to_le64(id | type << APFS_OBJ_TYPE_SHIFT);
}

/**
 * apfs_cat_type - Read the record type of a catalog key
 * @key: the raw catalog key
 */
static inline int apfs_cat_type(struct apfs_key_header *key)
{
	return (le64_to_cpu(key->obj_id_and_type) & APFS_OBJ_TYPE_MASK) >> APFS_OBJ_TYPE_SHIFT;
}

/**
 * apfs_cat_cnid - Read the cnid value on a catalog key
 * @key: the raw catalog key
 *
 * TODO: rename this function, since it's not just for the catalog anymore
 */
static inline u64 apfs_cat_cnid(struct apfs_key_header *key)
{
	return le64_to_cpu(key->obj_id_and_type) & APFS_OBJ_ID_MASK;
}

/* Flags for the query structure */
#define APFS_QUERY_TREE_MASK	000177	/* Which b-tree we query */
#define APFS_QUERY_OMAP		000001	/* This is a b-tree object map query */
#define APFS_QUERY_CAT		000002	/* This is a catalog tree query */
#define APFS_QUERY_FREE_QUEUE	000004	/* This is a free queue query */
#define APFS_QUERY_EXTENTREF	000010	/* This is an extent reference query */
#define APFS_QUERY_FEXT		000020	/* This is a fext tree query */
#define APFS_QUERY_SNAP_META	000040	/* This is a snapshot meta query */
#define APFS_QUERY_OMAP_SNAP	000100	/* This is an omap snapshots query */
#define APFS_QUERY_NEXT		000200	/* Find next of multiple matches */
#define APFS_QUERY_EXACT	000400	/* Search for an exact match */
#define APFS_QUERY_DONE		001000	/* The search at this level is over */
#define APFS_QUERY_ANY_NAME	002000	/* Multiple search for any name */
#define APFS_QUERY_ANY_NUMBER	004000	/* Multiple search for any number */
#define APFS_QUERY_MULTIPLE	(APFS_QUERY_ANY_NAME | APFS_QUERY_ANY_NUMBER)
#define APFS_QUERY_PREV		010000	/* Find previous record */

/*
 * Structure used to retrieve data from an APFS B-Tree. For now only used
 * on the calalog and the object map.
 */
struct apfs_query {
	struct apfs_node *node;		/* Node being searched */
	struct apfs_key key;		/* What the query is looking for */

	struct apfs_query *parent;	/* Query for parent node */
	unsigned int flags;

	/* Set by the query on success */
	int index;			/* Index of the entry in the node */
	int key_off;			/* Offset of the key in the node */
	int key_len;			/* Length of the key */
	int off;			/* Offset of the data in the node */
	int len;			/* Length of the data */

	int depth;			/* Put a limit on recursion */
};

/**
 * apfs_query_storage - Get the storage type for a query's btree
 * @query: the query structure
 */
static inline u32 apfs_query_storage(struct apfs_query *query)
{
	if (query->flags & APFS_QUERY_OMAP)
		return APFS_OBJ_PHYSICAL;
	if (query->flags & APFS_QUERY_CAT)
		return APFS_OBJ_VIRTUAL;
	if (query->flags & APFS_QUERY_FEXT)
		return APFS_OBJ_PHYSICAL;
	if (query->flags & APFS_QUERY_FREE_QUEUE)
		return APFS_OBJ_EPHEMERAL;
	if (query->flags & APFS_QUERY_EXTENTREF)
		return APFS_OBJ_PHYSICAL;
	if (query->flags & APFS_QUERY_SNAP_META)
		return APFS_OBJ_PHYSICAL;
	if (query->flags & APFS_QUERY_OMAP_SNAP)
		return APFS_OBJ_PHYSICAL;

	/* Absurd, but don't panic: let the callers fail and report it */
	return -1;
}

/*
 * Extent record data in memory
 */
struct apfs_file_extent {
	u64 logical_addr;
	u64 phys_block_num;
	u64 len;
	u64 crypto_id;
};

/*
 * Physical extent record data in memory
 */
struct apfs_phys_extent {
	u64 bno;
	u64 blkcount;
	u64 len;	/* In bytes */
	u32 refcnt;
	u8 kind;
};

/*
 * Data stream info in memory
 */
struct apfs_dstream_info {
	struct super_block	*ds_sb;		/* Filesystem superblock */
	struct inode		*ds_inode;	/* NULL for xattr dstreams */
	u64			ds_id;		/* ID of the extent records */
	u64			ds_size;	/* Length of the stream */
	u64			ds_sparse_bytes;/* Hole byte count in stream */
	struct apfs_file_extent	ds_cached_ext;	/* Latest extent record */
	bool			ds_ext_dirty;	/* Is ds_cached_ext dirty? */
	spinlock_t		ds_ext_lock;	/* Protects ds_cached_ext */
	bool			ds_shared;	/* Has multiple references? */
};

/**
 * apfs_alloced_size - Return the alloced size for a data stream
 * @dstream: data stream info
 *
 * TODO: is this always correct? Or could the extents have an unused tail?
 */
static inline u64 apfs_alloced_size(struct apfs_dstream_info *dstream)
{
	struct super_block *sb = dstream->ds_sb;
	u64 blks = (dstream->ds_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

	return blks << sb->s_blocksize_bits;
}

/*
 * APFS inode data in memory
 */
struct apfs_inode_info {
	u64			i_ino64;	 /* 32-bit-safe inode number */
	u64			i_parent_id;	 /* ID of primary parent */
	struct timespec64	i_crtime;	 /* Time of creation */
	u32			i_nchildren;	 /* Child count for directory */
	uid_t			i_saved_uid;	 /* User ID on disk */
	gid_t			i_saved_gid;	 /* Group ID on disk */
	u32			i_key_class;	 /* Security class for directory */
	u64			i_int_flags;	 /* Internal flags */
	u32			i_bsd_flags;	 /* BSD flags */
	struct list_head	i_list;		 /* List of inodes in transaction */

	bool			 i_has_dstream;	 /* Is there a dstream record? */
	struct apfs_dstream_info i_dstream;	 /* Dstream data, if any */

	bool			i_cleaned;	 /* Orphan data already deleted */

	struct inode vfs_inode;
};

static inline struct apfs_inode_info *APFS_I(const struct inode *inode)
{
	return container_of(inode, struct apfs_inode_info, vfs_inode);
}

/**
 * apfs_ino - Get the 64-bit id of an inode
 * @inode: the vfs inode
 *
 * Returns all 64 bits of @inode's id, even on 32-bit architectures.
 */
static inline u64 apfs_ino(const struct inode *inode)
{
	return APFS_I(inode)->i_ino64;
}

/**
 * apfs_set_ino - Set a 64-bit id on an inode
 * @inode: the vfs inode
 * @id:	   id to set
 *
 * Sets both the vfs inode number and the actual 32-bit-safe id.
 */
static inline void apfs_set_ino(struct inode *inode, u64 id)
{
	inode->i_ino = id; /* Higher bits may be lost, but it doesn't matter */
	APFS_I(inode)->i_ino64 = id;
}

/* Make the compiler complain if we ever access i_ino directly by mistake */
#define i_ino	DONT_USE_I_INO

/*
 * Directory entry record in memory
 */
struct apfs_drec {
	u8 *name;
	u64 ino;
	u64 sibling_id; /* The sibling id; 0 if none */
	int name_len;
	unsigned int type;
};

/*
 * Xattr record data in memory
 */
struct apfs_xattr {
	u8 *name;
	u8 *xdata;
	int name_len;
	int xdata_len;
	bool has_dstream;
};

struct apfs_compressed_data {
	bool has_dstream;
	u64 size;
	union {
		struct apfs_dstream_info *dstream;
		void *data;
	};
};

/*
 * Report function name and line number for the message types that are likely
 * to signal a bug, to make things easier for reporters. Don't do this for the
 * common messages, there is no point and it makes the console look too busy.
 */
#define apfs_emerg(sb, fmt, ...) apfs_msg(sb, KERN_EMERG, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define apfs_alert(sb, fmt, ...) apfs_msg(sb, KERN_ALERT, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define apfs_crit(sb, fmt, ...) apfs_msg(sb, KERN_CRIT, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define apfs_err(sb, fmt, ...) apfs_msg(sb, KERN_ERR, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define apfs_warn(sb, fmt, ...) apfs_msg(sb, KERN_WARNING, NULL, 0, fmt, ##__VA_ARGS__)
#define apfs_notice(sb, fmt, ...) apfs_msg(sb, KERN_NOTICE, NULL, 0, fmt, ##__VA_ARGS__)
#define apfs_info(sb, fmt, ...) apfs_msg(sb, KERN_INFO, NULL, 0, fmt, ##__VA_ARGS__)

#ifdef CONFIG_APFS_DEBUG
#define ASSERT(expr)	WARN_ON(!(expr))
#define apfs_debug(sb, fmt, ...) apfs_msg(sb, KERN_DEBUG, __func__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define ASSERT(expr)	((void)0)
#define apfs_debug(sb, fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif /* CONFIG_APFS_DEBUG */

/**
 * apfs_assert_in_transaction - Assert that the object is in current transaction
 * @sb:		superblock structure
 * @obj:	on-disk object to check
 */
#define apfs_assert_in_transaction(sb, obj)				\
do {									\
	(void)sb;							\
	(void)obj;							\
	ASSERT(le64_to_cpu((obj)->o_xid) == APFS_NXI(sb)->nx_xid);	\
} while (0)

/* btree.c */
extern struct apfs_node *apfs_query_root(const struct apfs_query *query);
extern struct apfs_query *apfs_alloc_query(struct apfs_node *node,
					   struct apfs_query *parent);
extern void apfs_free_query(struct apfs_query *query);
extern int apfs_btree_query(struct super_block *sb, struct apfs_query **query);
extern int apfs_omap_lookup_block(struct super_block *sb, struct apfs_omap *omap, u64 id, u64 *block, bool write);
extern int apfs_omap_lookup_newest_block(struct super_block *sb, struct apfs_omap *omap, u64 id, u64 *block, bool write);
extern int apfs_create_omap_rec(struct super_block *sb, u64 oid, u64 bno);
extern int apfs_delete_omap_rec(struct super_block *sb, u64 oid);
extern int apfs_query_join_transaction(struct apfs_query *query);
extern int __apfs_btree_insert(struct apfs_query *query, void *key, int key_len, void *val, int val_len);
extern int apfs_btree_insert(struct apfs_query *query, void *key, int key_len,
			     void *val, int val_len);
extern int apfs_btree_remove(struct apfs_query *query);
extern void apfs_btree_change_node_count(struct apfs_query *query, int change);
extern int apfs_btree_replace(struct apfs_query *query, void *key, int key_len,
			      void *val, int val_len);
extern void apfs_query_direct_forward(struct apfs_query *query);

/* compress.c */
extern int apfs_compress_get_size(struct inode *inode, loff_t *size);

/* dir.c */
extern int apfs_inode_by_name(struct inode *dir, const struct qstr *child,
			      u64 *ino);
extern int apfs_mkany(struct inode *dir, struct dentry *dentry,
		      umode_t mode, dev_t rdev, const char *symname);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
extern int apfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		      dev_t rdev);
extern int apfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
extern int apfs_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry,
		       unsigned int flags);
extern int apfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool excl);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
extern int apfs_mknod(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t rdev);
extern int apfs_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode);
extern int apfs_rename(struct user_namespace *mnt_userns, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags);
extern int apfs_create(struct user_namespace *mnt_userns, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl);
#else
extern int apfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t rdev);
extern int apfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode);
extern int apfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags);
extern int apfs_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl);
#endif

extern int apfs_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry);
extern int apfs_unlink(struct inode *dir, struct dentry *dentry);
extern int apfs_rmdir(struct inode *dir, struct dentry *dentry);
extern int apfs_delete_orphan_link(struct inode *inode);
extern int APFS_DELETE_ORPHAN_LINK_MAXOPS(void);
extern u64 apfs_any_orphan_ino(struct super_block *sb, u64 *ino_p);

/* extents.c */
extern int apfs_extent_from_query(struct apfs_query *query,
				  struct apfs_file_extent *extent);
extern int apfs_logic_to_phys_bno(struct apfs_dstream_info *dstream, sector_t dsblock, u64 *bno);
extern int __apfs_get_block(struct apfs_dstream_info *dstream, sector_t iblock,
			    struct buffer_head *bh_result, int create);
extern int apfs_get_block(struct inode *inode, sector_t iblock,
			  struct buffer_head *bh_result, int create);
extern int apfs_flush_extent_cache(struct apfs_dstream_info *dstream);
extern int apfs_dstream_get_new_bno(struct apfs_dstream_info *dstream, u64 dsblock, u64 *bno);
extern int apfs_get_new_block(struct inode *inode, sector_t iblock,
			      struct buffer_head *bh_result, int create);
extern int APFS_GET_NEW_BLOCK_MAXOPS(void);
extern int apfs_truncate(struct apfs_dstream_info *dstream, loff_t new_size);
extern int apfs_inode_delete_front(struct inode *inode);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
extern loff_t apfs_remap_file_range(struct file *src_file, loff_t off, struct file *dst_file, loff_t destoff, loff_t len, unsigned int remap_flags);
#else
extern int apfs_clone_file_range(struct file *src_file, loff_t off, struct file *dst_file, loff_t destoff, u64 len);
#endif
extern int apfs_clone_extents(struct apfs_dstream_info *dstream, u64 new_id);
extern int apfs_nonsparse_dstream_read(struct apfs_dstream_info *dstream, void *buf, size_t count, u64 offset);
extern void apfs_nonsparse_dstream_preread(struct apfs_dstream_info *dstream);

/* file.c */
extern int apfs_file_mmap(struct file *file, struct vm_area_struct *vma);
extern int apfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);

/* inode.c */
extern struct inode *apfs_iget(struct super_block *sb, u64 cnid);
extern int apfs_update_inode(struct inode *inode, char *new_name);
extern int APFS_UPDATE_INODE_MAXOPS(void);
extern void apfs_orphan_cleanup_work(struct work_struct *work);
extern void apfs_evict_inode(struct inode *inode);
extern struct inode *apfs_new_inode(struct inode *dir, umode_t mode,
				    dev_t rdev);
extern int apfs_create_inode_rec(struct super_block *sb, struct inode *inode,
				 struct dentry *dentry);
extern int apfs_inode_create_exclusive_dstream(struct inode *inode);
extern int APFS_CREATE_INODE_REC_MAXOPS(void);
extern int __apfs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int flags, struct page **pagep, void **fsdata);
extern int __apfs_write_end(struct file *file, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct page *page, void *fsdata);
extern int apfs_dstream_adj_refcnt(struct apfs_dstream_info *dstream, u32 delta);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
extern int apfs_setattr(struct dentry *dentry, struct iattr *iattr);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
extern int apfs_setattr(struct user_namespace *mnt_userns,
			struct dentry *dentry, struct iattr *iattr);
#else
extern int apfs_setattr(struct mnt_idmap *idmap,
			struct dentry *dentry, struct iattr *iattr);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
extern int apfs_update_time(struct inode *inode, struct timespec64 *time, int flags);
#else
extern int apfs_update_time(struct inode *inode, int flags);
#endif
long apfs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
long apfs_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) /* No statx yet... */
extern int apfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			struct kstat *stat);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
extern int apfs_getattr(const struct path *path, struct kstat *stat,
			u32 request_mask, unsigned int query_flags);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
extern int apfs_getattr(struct user_namespace *mnt_userns,
		const struct path *path, struct kstat *stat, u32 request_mask,
		unsigned int query_flags);
#else
extern int apfs_getattr(struct mnt_idmap *idmap,
		const struct path *path, struct kstat *stat, u32 request_mask,
		unsigned int query_flags);
#endif

extern int apfs_crypto_adj_refcnt(struct super_block *sb, u64 crypto_id, int delta);
extern int APFS_CRYPTO_ADJ_REFCNT_MAXOPS(void);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
extern int apfs_fileattr_get(struct dentry *dentry, struct fileattr *fa);
extern int apfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry, struct fileattr *fa);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
extern int apfs_fileattr_get(struct dentry *dentry, struct fileattr *fa);
extern int apfs_fileattr_set(struct user_namespace *mnt_userns, struct dentry *dentry, struct fileattr *fa);
#endif

/* key.c */
extern int apfs_filename_cmp(struct super_block *sb, const char *name1, unsigned int len1, const char *name2, unsigned int len2);
extern int apfs_keycmp(struct apfs_key *k1, struct apfs_key *k2);
extern int apfs_read_cat_key(void *raw, int size, struct apfs_key *key, bool hashed);
extern int apfs_read_fext_key(void *raw, int size, struct apfs_key *key);
extern int apfs_read_free_queue_key(void *raw, int size, struct apfs_key *key);
extern int apfs_read_omap_key(void *raw, int size, struct apfs_key *key);
extern int apfs_read_extentref_key(void *raw, int size, struct apfs_key *key);
extern int apfs_read_snap_meta_key(void *raw, int size, struct apfs_key *key);
extern int apfs_read_omap_snap_key(void *raw, int size, struct apfs_key *key);

/* message.c */
extern __printf(5, 6) void apfs_msg(struct super_block *sb, const char *prefix, const char *func, int line, const char *fmt, ...);

/* node.c */
extern struct apfs_node *apfs_read_node(struct super_block *sb, u64 oid,
					u32 storage, bool write);
extern void apfs_update_node(struct apfs_node *node);
extern int apfs_delete_node(struct apfs_node *node, int type);
extern int apfs_node_query(struct super_block *sb, struct apfs_query *query);
extern void apfs_node_query_first(struct apfs_query *query);
extern int apfs_omap_map_from_query(struct apfs_query *query, struct apfs_omap_map *map);
extern int apfs_node_split(struct apfs_query *query);
extern int apfs_node_locate_key(struct apfs_node *node, int index, int *off);
extern void apfs_node_free(struct apfs_node *node);
extern void apfs_node_free_range(struct apfs_node *node, u16 off, u16 len);
extern bool apfs_node_has_room(struct apfs_node *node, int length, bool replace);
extern int apfs_node_replace(struct apfs_query *query, void *key, int key_len, void *val, int val_len);
extern int apfs_node_insert(struct apfs_query *query, void *key, int key_len, void *val, int val_len);
extern int apfs_create_single_rec_node(struct apfs_query *query, void *key, int key_len, void *val, int val_len);
extern int apfs_make_empty_btree_root(struct super_block *sb, u32 subtype, u64 *oid);

/* object.c */
extern int apfs_obj_verify_csum(struct super_block *sb, struct buffer_head *bh);
extern void apfs_obj_set_csum(struct super_block *sb, struct apfs_obj_phys *obj);
extern int apfs_multiblock_verify_csum(char *object, u32 size);
extern void apfs_multiblock_set_csum(char *object, u32 size);
extern int apfs_create_cpm_block(struct super_block *sb, u64 bno, struct buffer_head **bh_p);
extern int apfs_create_cpoint_map(struct super_block *sb, struct apfs_checkpoint_map_phys *cpm, struct apfs_obj_phys *obj, u64 bno, u32 size);
extern struct apfs_ephemeral_object_info *apfs_ephemeral_object_lookup(struct super_block *sb, u64 oid);
extern struct buffer_head *apfs_read_object_block(struct super_block *sb, u64 bno, bool write, bool preserve);
extern u32 apfs_index_in_data_area(struct super_block *sb, u64 bno);
extern u64 apfs_data_index_to_bno(struct super_block *sb, u32 index);

/* snapshot.c */
extern int apfs_ioc_take_snapshot(struct file *file, void __user *user_arg);
extern int apfs_switch_to_snapshot(struct super_block *sb);

/* spaceman.c */
extern int apfs_read_spaceman(struct super_block *sb);
extern int apfs_free_queue_insert_nocache(struct super_block *sb, u64 bno, u64 count);
extern int apfs_free_queue_insert(struct super_block *sb, u64 bno, u64 count);
extern int apfs_spaceman_allocate_block(struct super_block *sb, u64 *bno, bool backwards);

/* super.c */
extern int apfs_map_volume_super_bno(struct super_block *sb, u64 bno, bool check);
extern int apfs_map_volume_super(struct super_block *sb, bool write);
extern void apfs_unmap_volume_super(struct super_block *sb);
extern int apfs_read_omap(struct super_block *sb, bool write);
extern int apfs_read_catalog(struct super_block *sb, bool write);
extern int apfs_sync_fs(struct super_block *sb, int wait);

/* transaction.c */
extern int apfs_cpoint_data_free(struct super_block *sb, u64 bno);
extern int apfs_transaction_start(struct super_block *sb, struct apfs_max_ops maxops);
extern int apfs_transaction_commit(struct super_block *sb);
extern void apfs_inode_join_transaction(struct super_block *sb, struct inode *inode);
extern int apfs_transaction_join(struct super_block *sb,
				 struct buffer_head *bh);
void apfs_transaction_abort(struct super_block *sb);
extern int apfs_transaction_flush_all_inodes(struct super_block *sb);

/* xattr.c */
extern int ____apfs_xattr_get(struct inode *inode, const char *name, void *buffer,
			      size_t size, bool only_whole);
extern int __apfs_xattr_get(struct inode *inode, const char *name, void *buffer,
			    size_t size);
extern int apfs_delete_all_xattrs(struct inode *inode);
extern int apfs_xattr_set(struct inode *inode, const char *name, const void *value,
			  size_t size, int flags);
extern int APFS_XATTR_SET_MAXOPS(void);
extern ssize_t apfs_listxattr(struct dentry *dentry, char *buffer, size_t size);
extern int apfs_xattr_get_compressed_data(struct inode *inode, const char *name, struct apfs_compressed_data *cdata);
extern void apfs_release_compressed_data(struct apfs_compressed_data *cdata);
extern int apfs_compressed_data_read(struct apfs_compressed_data *cdata, void *buf, size_t count, u64 offset);

/* xfield.c */
extern int apfs_find_xfield(u8 *xfields, int len, u8 xtype, char **xval);
extern int apfs_init_xfields(u8 *buffer, int buflen);
extern int apfs_insert_xfield(u8 *buffer, int buflen,
			      const struct apfs_x_field *xkey,
			      const void *xval);

/*
 * Inode and file operations
 */

/* compress.c */
extern const struct address_space_operations apfs_compress_aops;
extern const struct file_operations apfs_compress_file_operations;

/* dir.c */
extern const struct file_operations apfs_dir_operations;

/* file.c */
extern const struct file_operations apfs_file_operations;
extern const struct inode_operations apfs_file_inode_operations;

/* namei.c */
extern const struct inode_operations apfs_dir_inode_operations;
extern const struct inode_operations apfs_special_inode_operations;
extern const struct dentry_operations apfs_dentry_operations;

/* symlink.c */
extern const struct inode_operations apfs_symlink_inode_operations;

/* xattr.c */
extern const struct xattr_handler *apfs_xattr_handlers[];

/**
 * apfs_assert_query_is_valid - Assert that all of a query's ancestors are set
 * @query: the query to check
 *
 * A query may lose some of its ancestors during a node split, but nothing
 * should be done to such a query until it gets refreshed.
 */
static inline void apfs_assert_query_is_valid(const struct apfs_query *query)
{
	ASSERT(apfs_node_is_root(apfs_query_root(query)));
}

/*
 * TODO: the following are modified variants of buffer head functions that will
 * work with the shared block device for the container. The correct approach
 * here would be to avoid buffer heads and use bios, but for now this will do.
 */

static inline void
apfs_map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block)
{
	set_buffer_mapped(bh);
	bh->b_bdev = APFS_NXI(sb)->nx_bdev;
	bh->b_blocknr = block;
	bh->b_size = sb->s_blocksize;
}

static inline struct buffer_head *
apfs_sb_bread(struct super_block *sb, sector_t block)
{
	return __bread_gfp(APFS_NXI(sb)->nx_bdev, block, sb->s_blocksize, __GFP_MOVABLE);
}

/* Use instead of apfs_sb_bread() for blocks that will just be overwritten */
static inline struct buffer_head *
apfs_getblk(struct super_block *sb, sector_t block)
{
	struct buffer_head *bh;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	bh = __getblk_gfp(APFS_NXI(sb)->nx_bdev, block, sb->s_blocksize, __GFP_MOVABLE);
#else
	bh = bdev_getblk(APFS_NXI(sb)->nx_bdev, block, sb->s_blocksize, __GFP_MOVABLE);
#endif
	if (bh)
		set_buffer_uptodate(bh);
	return bh;
}

#endif	/* _APFS_H */
