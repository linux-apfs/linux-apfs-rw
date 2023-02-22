/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 *
 * Definitions for all on-disk data structures.
 */

#ifndef _APFS_RAW_H
#define _APFS_RAW_H

#include <linux/types.h>
#include <linux/uuid.h>

/* Object identifiers constants */
#define APFS_OID_NX_SUPERBLOCK			1
#define APFS_OID_INVALID			0ULL
#define APFS_OID_RESERVED_COUNT			1024

/* Object type masks */
#define APFS_OBJECT_TYPE_MASK			0x0000ffff
#define APFS_OBJECT_TYPE_FLAGS_MASK		0xffff0000
#define APFS_OBJ_STORAGETYPE_MASK		0xc0000000
#define APFS_OBJECT_TYPE_FLAGS_DEFINED_MASK	0xf8000000

/* Object types */
#define APFS_OBJECT_TYPE_NX_SUPERBLOCK		0x00000001
#define APFS_OBJECT_TYPE_BTREE			0x00000002
#define APFS_OBJECT_TYPE_BTREE_NODE		0x00000003
#define APFS_OBJECT_TYPE_SPACEMAN		0x00000005
#define APFS_OBJECT_TYPE_SPACEMAN_CAB		0x00000006
#define APFS_OBJECT_TYPE_SPACEMAN_CIB		0x00000007
#define APFS_OBJECT_TYPE_SPACEMAN_BITMAP	0x00000008
#define APFS_OBJECT_TYPE_SPACEMAN_FREE_QUEUE	0x00000009
#define APFS_OBJECT_TYPE_EXTENT_LIST_TREE	0x0000000a
#define APFS_OBJECT_TYPE_OMAP			0x0000000b
#define APFS_OBJECT_TYPE_CHECKPOINT_MAP		0x0000000c
#define APFS_OBJECT_TYPE_FS			0x0000000d
#define APFS_OBJECT_TYPE_FSTREE			0x0000000e
#define APFS_OBJECT_TYPE_BLOCKREFTREE		0x0000000f
#define APFS_OBJECT_TYPE_SNAPMETATREE		0x00000010
#define APFS_OBJECT_TYPE_NX_REAPER		0x00000011
#define APFS_OBJECT_TYPE_NX_REAP_LIST		0x00000012
#define APFS_OBJECT_TYPE_OMAP_SNAPSHOT		0x00000013
#define APFS_OBJECT_TYPE_EFI_JUMPSTART		0x00000014
#define APFS_OBJECT_TYPE_FUSION_MIDDLE_TREE	0x00000015
#define APFS_OBJECT_TYPE_NX_FUSION_WBC		0x00000016
#define APFS_OBJECT_TYPE_NX_FUSION_WBC_LIST	0x00000017
#define APFS_OBJECT_TYPE_ER_STATE		0x00000018
#define APFS_OBJECT_TYPE_GBITMAP		0x00000019
#define APFS_OBJECT_TYPE_GBITMAP_TREE		0x0000001a
#define APFS_OBJECT_TYPE_GBITMAP_BLOCK		0x0000001b
#define APFS_OBJECT_TYPE_ER_RECOVERY_BLOCK	0x0000001c
#define APFS_OBJECT_TYPE_SNAP_META_EXT		0x0000001d
#define APFS_OBJECT_TYPE_INTEGRITY_META		0x0000001e
#define APFS_OBJECT_TYPE_FEXT_TREE		0x0000001f
#define APFS_OBJECT_TYPE_RESERVED_20		0x00000020
#define APFS_OBJECT_TYPE_INVALID		0x00000000
#define APFS_OBJECT_TYPE_TEST			0x000000ff

/* Object type flags */
#define APFS_OBJ_VIRTUAL			0x00000000
#define APFS_OBJ_EPHEMERAL			0x80000000
#define APFS_OBJ_PHYSICAL			0x40000000
#define APFS_OBJ_NOHEADER			0x20000000
#define APFS_OBJ_ENCRYPTED			0x10000000
#define APFS_OBJ_NONPERSISTENT			0x08000000

#define APFS_MAX_CKSUM_SIZE 8

/*
 * On-disk representation of an APFS object
 */
struct apfs_obj_phys {
/*00*/	__le64 o_cksum;		/* Fletcher checksum */
	__le64 o_oid;		/* Object-id */
/*10*/	__le64 o_xid;		/* Transaction ID */
	__le32 o_type;		/* Object type */
	__le32 o_subtype;	/* Object subtype */
} __packed;

/* Flags for the object map structure */
#define APFS_OMAP_MANUALLY_MANAGED	0x00000001
#define APFS_OMAP_ENCRYPTING		0x00000002
#define APFS_OMAP_DECRYPTING		0x00000004
#define APFS_OMAP_KEYROLLING		0x00000008
#define APFS_OMAP_CRYPTO_GENERATION	0x00000010
#define APFS_OMAP_FLAGS_VALID_MASK	(APFS_OMAP_MANUALLY_MANAGED \
					| APFS_OMAP_ENCRYPTING \
					| APFS_OMAP_DECRYPTING \
					| APFS_OMAP_KEYROLLING \
					| APFS_OMAP_CRYPTO_GENERATION)

/*
 * On-disk representation of an object map
 */
struct apfs_omap_phys {
/*00*/	struct apfs_obj_phys om_o;
/*20*/	__le32 om_flags;
	__le32 om_snap_count;
	__le32 om_tree_type;
	__le32 om_snapshot_tree_type;
/*30*/	__le64 om_tree_oid;
	__le64 om_snapshot_tree_oid;
/*40*/	__le64 om_most_recent_snap;
	__le64 om_pending_revert_min;
	__le64 om_pending_revert_max;
} __packed;

/* Object map value flags */
#define APFS_OMAP_VAL_DELETED		0x00000001
#define APFS_OMAP_VAL_SAVED		0x00000002
#define APFS_OMAP_VAL_ENCRYPTED		0x00000004
#define APFS_OMAP_VAL_NOHEADER		0x00000008
#define APFS_OMAP_VAL_CRYPTO_GENERATION	0x00000010
#define APFS_OMAP_VAL_FLAGS_VALID_MASK	(APFS_OMAP_VAL_DELETED \
					| APFS_OMAP_VAL_SAVED \
					| APFS_OMAP_VAL_ENCRYPTED \
					| APFS_OMAP_VAL_NOHEADER \
					| APFS_OMAP_VAL_CRYPTO_GENERATION)

/*
 * Structure of a value in an object map B-tree
 */
struct apfs_omap_val {
	__le32 ov_flags;
	__le32 ov_size;
	__le64 ov_paddr;
} __packed;

/*
 * Structure of a value in an omap's snapshot tree
 */
struct apfs_omap_snapshot {
	__le32 oms_flags;
	__le32 oms_pad;
	__le64 oms_oid;
} __packed;

/* B-tree node flags */
#define APFS_BTNODE_ROOT		0x0001
#define APFS_BTNODE_LEAF		0x0002
#define APFS_BTNODE_FIXED_KV_SIZE	0x0004
#define APFS_BTNODE_CHECK_KOFF_INVAL	0x8000
#define APFS_BTNODE_MASK		0x0007	/* Valid on-disk flags */

/* B-tree location constants */
#define APFS_BTOFF_INVALID		0xffff

/*
 * Structure storing a location inside a B-tree node
 */
struct apfs_nloc {
	__le16 off;
	__le16 len;
} __packed;

/*
 * Structure storing the location of a key/value pair within a B-tree node
 */
struct apfs_kvloc {
	struct apfs_nloc k;
	struct apfs_nloc v;
} __packed;

/*
 * Structure storing the location of a key/value pair within a B-tree node
 * having fixed-size key and value (flag APFS_BTNODE_FIXED_KV_SIZE is present)
 */
struct apfs_kvoff {
	__le16 k;
	__le16 v;
} __packed;

/*
 * On-disk representation of a B-tree node
 */
struct apfs_btree_node_phys {
/*00*/	struct apfs_obj_phys btn_o;
/*20*/	__le16 btn_flags;
	__le16 btn_level;
	__le32 btn_nkeys;
/*28*/	struct apfs_nloc btn_table_space;
	struct apfs_nloc btn_free_space;
	struct apfs_nloc btn_key_free_list;
	struct apfs_nloc btn_val_free_list;
/*38*/	__le64 btn_data[];
} __packed;

/* B-tree info flags */
#define APFS_BTREE_UINT64_KEYS		0x00000001
#define APFS_BTREE_SEQUENTIAL_INSERT	0x00000002
#define APFS_BTREE_ALLOW_GHOSTS		0x00000004
#define APFS_BTREE_EPHEMERAL		0x00000008
#define APFS_BTREE_PHYSICAL		0x00000010
#define APFS_BTREE_NONPERSISTENT	0x00000020
#define APFS_BTREE_KV_NONALIGNED	0x00000040
#define APFS_BTREE_FLAGS_VALID_MASK	(APFS_BTREE_UINT64_KEYS \
					| APFS_BTREE_SEQUENTIAL_INSERT \
					| APFS_BTREE_ALLOW_GHOSTS \
					| APFS_BTREE_EPHEMERAL \
					| APFS_BTREE_PHYSICAL \
					| APFS_BTREE_NONPERSISTENT \
					| APFS_BTREE_KV_NONALIGNED)

/*
 * Structure used to store information about a B-tree that won't change
 * over time
 */
struct apfs_btree_info_fixed {
	__le32 bt_flags;
	__le32 bt_node_size;
	__le32 bt_key_size;
	__le32 bt_val_size;
} __packed;

/*
 * Structure used to store information about a B-tree (located at the end of
 * a B-tree root node block)
 */
struct apfs_btree_info {
	struct apfs_btree_info_fixed bt_fixed;
	__le32 bt_longest_key;			/* Longest key ever stored */
	__le32 bt_longest_val;			/* Longest value ever stored */
	__le64 bt_key_count;
	__le64 bt_node_count;
} __packed;

/*
 * Structure of the value of a directory entry. This is the data in
 * the catalog nodes for record type APFS_TYPE_DIR_REC.
 */
struct apfs_drec_val {
	__le64 file_id;
	__le64 date_added;
	__le16 flags;
	u8 xfields[];
} __packed;

/* Physical extent records */
#define APFS_PEXT_LEN_MASK	0x0fffffffffffffffULL
#define APFS_PEXT_KIND_MASK	0xf000000000000000ULL
#define APFS_PEXT_KIND_SHIFT	60

/* The kind of a physical extent record */
enum {
	APFS_KIND_ANY		= 0,
	APFS_KIND_NEW		= 1,
	APFS_KIND_UPDATE	= 2,
	APFS_KIND_DEAD		= 3,
	APFS_KIND_UPDATE_REFCNT	= 4,

	APFS_KIND_INVALID	= 255 /* This is weird, won't fit in 4 bits */
};

#define APFS_OWNING_OBJ_ID_INVALID	(~0ULL)
#define APFS_OWNING_OBJ_ID_UNKNOWN	(~1ULL)

/*
 * Structure of a physical extent record
 */
struct apfs_phys_ext_val {
	__le64 len_and_kind;
	__le64 owning_obj_id;
	__le32 refcnt;
} __packed;

/* File extent records */
#define APFS_FILE_EXTENT_LEN_MASK	0x00ffffffffffffffULL
#define APFS_FILE_EXTENT_FLAG_MASK	0xff00000000000000ULL
#define APFS_FILE_EXTENT_FLAG_SHIFT	56

/*
 * Put a bound on maximum file size so that a growing truncation will always
 * produce a single hole extent, even if 64k block sizes were in use. Larger
 * file sizes could be supported with multiple extents of course, but it takes
 * some work and I don't see the point.
 */
#define APFS_MAX_FILE_SIZE		0x00ffffffffff0000ULL

/*
 * Structure of a file extent record
 */
struct apfs_file_extent_val {
	__le64 len_and_flags;
	__le64 phys_block_num;
	__le64 crypto_id;
} __packed;

/*
 * Structure of a data stream record
 */
struct apfs_dstream_id_val {
	__le32 refcnt;
} __packed;

#define APFS_CP_MAX_WRAPPEDKEYSIZE	128

/*
 * Structure used to store the encryption state for PFKs
 */
struct apfs_wrapped_crypto_state {
	__le16 major_version;
	__le16 minor_version;
	__le32 cpflags;
	__le32 persistent_class;
	__le32 key_os_version;
	__le16 key_revision;
	__le16 key_len;
	u8 persistent_key[0];
} __packed;

/*
 * Structure of a crypto state record
 */
struct apfs_crypto_state_val {
	__le32 refcnt;
	struct apfs_wrapped_crypto_state state;
} __packed;

/* Inode numbers for special inodes */
#define APFS_INVALID_INO_NUM		0

#define APFS_ROOT_DIR_PARENT		1	/* Root directory parent */
#define APFS_ROOT_DIR_INO_NUM		2	/* Root directory */
#define APFS_PRIV_DIR_INO_NUM		3	/* Private directory */
#define APFS_SNAP_DIR_INO_NUM		6	/* Snapshots metadata */
#define APFS_PURGEABLE_DIR_INO_NUM	7	/* Parent of purgeable files */

/* Smallest inode number available for user content */
#define APFS_MIN_USER_INO_NUM		16

#define APFS_UNIFIED_ID_SPACE_MARK	0x0800000000000000

/* Inode internal flags */
#define APFS_INODE_IS_APFS_PRIVATE		0x00000001
#define APFS_INODE_MAINTAIN_DIR_STATS		0x00000002
#define APFS_INODE_DIR_STATS_ORIGIN		0x00000004
#define APFS_INODE_PROT_CLASS_EXPLICIT		0x00000008
#define APFS_INODE_WAS_CLONED			0x00000010
#define APFS_INODE_FLAG_UNUSED			0x00000020
#define APFS_INODE_HAS_SECURITY_EA		0x00000040
#define APFS_INODE_BEING_TRUNCATED		0x00000080
#define APFS_INODE_HAS_FINDER_INFO		0x00000100
#define APFS_INODE_IS_SPARSE			0x00000200
#define APFS_INODE_WAS_EVER_CLONED		0x00000400
#define APFS_INODE_ACTIVE_FILE_TRIMMED		0x00000800
#define APFS_INODE_PINNED_TO_MAIN		0x00001000
#define APFS_INODE_PINNED_TO_TIER2		0x00002000
#define APFS_INODE_HAS_RSRC_FORK		0x00004000
#define APFS_INODE_NO_RSRC_FORK			0x00008000
#define APFS_INODE_ALLOCATION_SPILLEDOVER	0x00010000
#define APFS_INODE_FAST_PROMOTE			0x00020000
#define APFS_INODE_HAS_UNCOMPRESSED_SIZE	0x00040000
#define APFS_INODE_IS_PURGEABLE			0x00080000
#define APFS_INODE_WANTS_TO_BE_PURGEABLE	0x00100000
#define APFS_INODE_IS_SYNC_ROOT			0x00200000
#define APFS_INODE_SNAPSHOT_COW_EXEMPTION	0x00400000
/* This flag is not documented */
#define APFS_INODE_HAS_PURGEABLE_FLAGS		0x02000000

/* Masks for internal flags */
#define APFS_VALID_INTERNAL_INODE_FLAGS	(APFS_INODE_IS_APFS_PRIVATE \
					| APFS_INODE_MAINTAIN_DIR_STATS \
					| APFS_INODE_DIR_STATS_ORIGIN \
					| APFS_INODE_PROT_CLASS_EXPLICIT \
					| APFS_INODE_WAS_CLONED \
					| APFS_INODE_HAS_SECURITY_EA \
					| APFS_INODE_BEING_TRUNCATED \
					| APFS_INODE_HAS_FINDER_INFO \
					| APFS_INODE_IS_SPARSE \
					| APFS_INODE_WAS_EVER_CLONED \
					| APFS_INODE_ACTIVE_FILE_TRIMMED \
					| APFS_INODE_PINNED_TO_MAIN \
					| APFS_INODE_PINNED_TO_TIER2 \
					| APFS_INODE_HAS_RSRC_FORK \
					| APFS_INODE_NO_RSRC_FORK \
					| APFS_INODE_ALLOCATION_SPILLEDOVER \
					| APFS_INODE_FAST_PROMOTE \
					| APFS_INODE_HAS_UNCOMPRESSED_SIZE \
					| APFS_INODE_IS_PURGEABLE \
					| APFS_INODE_WANTS_TO_BE_PURGEABLE \
					| APFS_INODE_IS_SYNC_ROOT \
					| APFS_INODE_SNAPSHOT_COW_EXEMPTION \
					| APFS_INODE_HAS_PURGEABLE_FLAGS)
#define APFS_INODE_INHERITED_INTERNAL_FLAGS	(APFS_INODE_MAINTAIN_DIR_STATS)
#define APFS_INDOE_CLONED_INTERNAL_FLAGS	(APFS_INODE_HAS_RSRC_FORK \
						| APFS_INODE_NO_RSRC_FORK \
						| APFS_INODE_HAS_FINDER_INFO)
#define APFS_INODE_PINNED_MASK			(APFS_INODE_PINNED_TO_MAIN \
						| APFS_INODE_PINNED_TO_TIER2)

/* BSD flags */
#define APFS_INOBSD_NODUMP			0x00000001
#define APFS_INOBSD_IMMUTABLE			0x00000002
#define APFS_INOBSD_APPEND			0x00000004
#define APFS_INOBSD_COMPRESSED			0x00000020

/*
 * Structure of an inode as stored as a B-tree value
 */
struct apfs_inode_val {
/*00*/	__le64 parent_id;
	__le64 private_id;
/*10*/	__le64 create_time;
	__le64 mod_time;
	__le64 change_time;
	__le64 access_time;
/*30*/	__le64 internal_flags;
	union {
		__le32 nchildren;
		__le32 nlink;
	};
	__le32 default_protection_class;
/*40*/	__le32 write_generation_counter;
	__le32 bsd_flags;
	__le32 owner;
	__le32 group;
/*50*/	__le16 mode;
	__le16 pad1;
	__le64 uncompressed_size;
/*5C*/	u8 xfields[];
} __packed;

/* Extended field types for dentries */
#define APFS_DREC_EXT_TYPE_SIBLING_ID 1

/* Extended field types for inodes */
#define APFS_INO_EXT_TYPE_SNAP_XID 1
#define APFS_INO_EXT_TYPE_DELTA_TREE_OID 2
#define APFS_INO_EXT_TYPE_DOCUMENT_ID 3
#define APFS_INO_EXT_TYPE_NAME 4
#define APFS_INO_EXT_TYPE_PREV_FSIZE 5
#define APFS_INO_EXT_TYPE_RESERVED_6 6
#define APFS_INO_EXT_TYPE_FINDER_INFO 7
#define APFS_INO_EXT_TYPE_DSTREAM 8
#define APFS_INO_EXT_TYPE_RESERVED_9 9
#define APFS_INO_EXT_TYPE_DIR_STATS_KEY 10
#define APFS_INO_EXT_TYPE_FS_UUID 11
#define APFS_INO_EXT_TYPE_RESERVED_12 12
#define APFS_INO_EXT_TYPE_SPARSE_BYTES 13
#define APFS_INO_EXT_TYPE_RDEV 14
#define APFS_INO_EXT_TYPE_PURGEABLE_FLAGS 15
#define APFS_INO_EXT_TYPE_ORIG_SYNC_ROOT_ID 16

/* Extended field flags */
#define APFS_XF_DATA_DEPENDENT		0x01
#define APFS_XF_DO_NOT_COPY		0x02
#define APFS_XF_RESERVED_4		0x04
#define APFS_XF_CHILDREN_INHERIT	0x08
#define APFS_XF_USER_FIELD		0x10
#define APFS_XF_SYSTEM_FIELD		0x20
#define APFS_XF_RESERVED_40		0x40
#define APFS_XF_RESERVED_80		0x80

/* Constants for extended fields */
#define APFS_MIN_DOC_ID 3	/* Smallest not reserved document id */

/*
 * Structure used to store the number and size of an xfield collection.  The
 * official reference seems to be wrong about @xf_used_data: it's the size of
 * the xfield values alone, without the metadata.
 */
struct apfs_xf_blob {
	__le16 xf_num_exts;
	__le16 xf_used_data;
	u8 xf_data[];
} __packed;

/*
 * Structure used to describe an extended field
 */
struct apfs_x_field {
	u8 x_type;
	u8 x_flags;
	__le16 x_size;
} __packed;

/*
 * Structure used to store information about a data stream
 */
struct apfs_dstream {
	__le64 size;
	__le64 alloced_size;
	__le64 default_crypto_id;
	__le64 total_bytes_written;
	__le64 total_bytes_read;
} __packed;

/*
 * Structure used to store directory information
 */
struct apfs_dir_stats_val {
	__le64 num_children;
	__le64 total_size;
	__le64 chained_key;
	__le64 gen_count;
} __packed;

/*
 * Structure of the value for a sibling link record.  These are used to
 * list the hard links for a given inode.
 */
struct apfs_sibling_val {
	__le64 parent_id;
	__le16 name_len;
	u8 name[0];
} __packed;

/*
 * Structure of the value for a sibling map record.  No idea what these are for.
 */
struct apfs_sibling_map_val {
	__le64 file_id;
} __packed;

/*
 * Structure of a key in an object map B-tree
 */
struct apfs_omap_key {
	__le64 ok_oid;
	__le64 ok_xid;
} __packed;

/*
 * Structure of a key in a free-space queue b-tree
 */
struct apfs_spaceman_free_queue_key {
	__le64 sfqk_xid;
	__le64 sfqk_paddr;
} __packed;

/* Catalog records types */
enum {
	APFS_TYPE_ANY			= 0,
	APFS_TYPE_SNAP_METADATA		= 1,
	APFS_TYPE_EXTENT		= 2,
	APFS_TYPE_INODE			= 3,
	APFS_TYPE_XATTR			= 4,
	APFS_TYPE_SIBLING_LINK		= 5,
	APFS_TYPE_DSTREAM_ID		= 6,
	APFS_TYPE_CRYPTO_STATE		= 7,
	APFS_TYPE_FILE_EXTENT		= 8,
	APFS_TYPE_DIR_REC		= 9,
	APFS_TYPE_DIR_STATS		= 10,
	APFS_TYPE_SNAP_NAME		= 11,
	APFS_TYPE_SIBLING_MAP		= 12,
	APFS_TYPE_MAX_VALID		= 12,
	APFS_TYPE_MAX			= 15,
	APFS_TYPE_INVALID		= 15,
};

/* Bit masks for the 'obj_id_and_type' field of a key header */
#define APFS_OBJ_ID_MASK		0x0fffffffffffffffULL
#define APFS_OBJ_TYPE_MASK		0xf000000000000000ULL
#define APFS_OBJ_TYPE_SHIFT		60

/* Key header for filesystem-object keys */
struct apfs_key_header {
	__le64 obj_id_and_type;
} __packed;

/*
 * Structure of the key for a physical extent record
 */
struct apfs_phys_ext_key {
	struct apfs_key_header hdr;
} __packed;

/*
 * Structure of the key for an inode record
 */
struct apfs_inode_key {
	struct apfs_key_header hdr;
} __packed;

/*
 * Structure of the key for a file extent record
 */
struct apfs_file_extent_key {
	struct apfs_key_header hdr;
	__le64 logical_addr;
} __packed;

/*
 * Structure of the key for a data stream record
 */
struct apfs_dstream_id_key {
	struct apfs_key_header hdr;
} __packed;

/*
 * Structure of the key for a crypto state record
 */
struct apfs_crypto_state_key {
	struct apfs_key_header hdr;
} __packed;

/* Bit masks for the 'name_len_and_hash' field of a directory entry */
#define APFS_DREC_LEN_MASK	0x000003ff
#define APFS_DREC_HASH_MASK	0xfffffc00
#define APFS_DREC_HASH_SHIFT	10

/* We could support bigger filenames, but I don't know if Linux allows it */
#define APFS_NAME_LEN		NAME_MAX

/* Bit masks for the 'type' field of a directory entry */
enum {
	APFS_DREC_TYPE_MASK	= 0x000f,
	APFS_DREC_RESERVED_10	= 0x0010,

	/* These flags are not documented */
	APFS_DREC_PURGEABLE_2	= 0x0200,
	APFS_DREC_PURGEABLE_8	= 0x0800,
};

#define APFS_DREC_PURGEABLE	(APFS_DREC_PURGEABLE_2 | APFS_DREC_PURGEABLE_8)

/*
 * Structure of the key for a directory entry - no hash, used on normalization
 * sensitive volumes
 */
struct apfs_drec_key {
	struct apfs_key_header hdr;
	__le16 name_len;
	u8 name[0];
} __packed;

/*
 * Structure of the key for a directory entry, including a precomputed
 * hash of its name
 */
struct apfs_drec_hashed_key {
	struct apfs_key_header hdr;
	__le32 name_len_and_hash;
	u8 name[0];
} __packed;

/*
 * Structure of the key for an extended attributes record
 */
struct apfs_xattr_key {
	struct apfs_key_header hdr;
	__le16 name_len;
	u8 name[0];
} __packed;

/*
 * Structure of the key for a snapshot metadata record
 */
struct apfs_snap_metadata_key {
	struct apfs_key_header hdr;
} __packed;

/* The snapshot name headers always have this placeholder object id */
#define APFS_SNAP_NAME_OBJ_ID	(~0ULL & APFS_OBJ_ID_MASK)

/*
 * Structure of the key for a snapshot name record
 */
struct apfs_snap_name_key {
	struct apfs_key_header hdr;
	__le16 name_len;
	u8 name[0];
} __packed;

/*
 * Structure of the key for a sibling link record
 */
struct apfs_sibling_link_key {
	struct apfs_key_header hdr;
	__le64 sibling_id;
} __packed;

/*
 * Structure of the key for a siblink map record
 */
struct apfs_sibling_map_key {
	struct apfs_key_header hdr;
} __packed;

/*
 * On-disk allocation info for a chunk of blocks
 */
struct apfs_chunk_info {
	__le64	ci_xid;
	__le64	ci_addr;
	__le32	ci_block_count;
	__le32	ci_free_count;
	__le64	ci_bitmap_addr;
} __packed;

/* Constants for the chunk info block */
#define	APFS_CI_COUNT_MASK		0x000FFFFF
#define	APFS_CI_COUNT_RESERVED_MASK	0xFFF00000

/*
 * Structure of a block with an array of chunk allocation info structures
 */
struct apfs_chunk_info_block {
	struct apfs_obj_phys	cib_o;
	__le32			cib_index;
	__le32			cib_chunk_info_count;
	struct apfs_chunk_info	cib_chunk_info[];
} __packed;

/*
 * Structure of a block with an array of addresses to chunk information blocks
 */
struct apfs_cib_addr_block {
	struct apfs_obj_phys	cab_o;
	__le32			cab_index;
	__le32			cab_cib_count;
	__le64			cab_cib_addr[];
} __packed;

/*
 * On-disk structure for a free queue
 */
struct apfs_spaceman_free_queue {
	__le64	sfq_count;
	__le64	sfq_tree_oid;
	__le64	sfq_oldest_xid;
	__le16	sfq_tree_node_limit;
	__le16	sfq_pad16;
	__le32	sfq_pad32;
	__le64	sfq_reserved;
} __packed;

/* Indexes for a free queue array */
enum {
	APFS_SFQ_IP	= 0,
	APFS_SFQ_MAIN	= 1,
	APFS_SFQ_TIER2	= 2,
	APFS_SFQ_COUNT	= 3
};

/*
 * On-disk structure for device allocation information
 */
struct apfs_spaceman_device {
	__le64	sm_block_count;
	__le64	sm_chunk_count;
	__le32	sm_cib_count;
	__le32	sm_cab_count;
	__le64	sm_free_count;
	__le32	sm_addr_offset;
	__le32	sm_reserved;
	__le64	sm_reserved2;
} __packed;

/* Indexes for a device array */
enum {
	APFS_SD_MAIN	= 0,
	APFS_SD_TIER2	= 1,
	APFS_SD_COUNT	= 2
};

/*
 * On-disk structure to describe allocation zone boundaries
 */
struct apfs_spaceman_allocation_zone_boundaries {
	__le64	saz_zone_start;
	__le64	saz_zone_end;
} __packed;

/* Allocation zone constants */
#define	APFS_SM_ALLOCZONE_INVALID_END_BOUNDARY		0
#define	APFS_SM_ALLOCZONE_NUM_PREVIOUS_BOUNDARIES	7

struct apfs_spaceman_allocation_zone_info_phys {
	struct apfs_spaceman_allocation_zone_boundaries	saz_current_boundaries;
	struct apfs_spaceman_allocation_zone_boundaries
	     saz_previous_boundaries[APFS_SM_ALLOCZONE_NUM_PREVIOUS_BOUNDARIES];

	__le16	saz_zone_id;
	__le16	saz_previous_boundary_index;
	__le32	saz_reserved;
} __packed;

/* Datazone constants */
#define	APFS_SM_DATAZONE_ALLOCZONE_COUNT	8

struct apfs_spaceman_datazone_info_phys {
	struct apfs_spaceman_allocation_zone_info_phys
	  sdz_allocation_zones[APFS_SD_COUNT][APFS_SM_DATAZONE_ALLOCZONE_COUNT];
} __packed;

/* Internal-pool bitmap constants */
#define	APFS_SPACEMAN_IP_BM_TX_MULTIPLIER	16
#define	APFS_SPACEMAN_IP_BM_INDEX_INVALID	0xFFFF
#define	APFS_SPACEMAN_IP_BM_BLOCK_COUNT_MAX	0xFFFE

/* Space manager flags */
#define	APFS_SM_FLAG_VERSIONED		0x00000001
#define	APFS_SM_FLAGS_VALID_MASK	APFS_SM_FLAG_VERSIONED

/*
 * On-disk structure for the space manager
 */
struct apfs_spaceman_phys {
	struct apfs_obj_phys			sm_o;
	__le32					sm_block_size;
	__le32					sm_blocks_per_chunk;
	__le32					sm_chunks_per_cib;
	__le32					sm_cibs_per_cab;
	struct apfs_spaceman_device		sm_dev[APFS_SD_COUNT];
	__le32					sm_flags;
	__le32					sm_ip_bm_tx_multiplier;
	__le64					sm_ip_block_count;
	__le32					sm_ip_bm_size_in_blocks;
	__le32					sm_ip_bm_block_count;
	__le64					sm_ip_bm_base;
	__le64					sm_ip_base;
	__le64					sm_fs_reserve_block_count;
	__le64					sm_fs_reserve_alloc_count;
	struct apfs_spaceman_free_queue		sm_fq[APFS_SFQ_COUNT];
	__le16					sm_ip_bm_free_head;
	__le16					sm_ip_bm_free_tail;
	__le32					sm_ip_bm_xid_offset;
	__le32					sm_ip_bitmap_offset;
	__le32					sm_ip_bm_free_next_offset;
	__le32					sm_version;
	__le32					sm_struct_size;
	struct apfs_spaceman_datazone_info_phys	sm_datazone;
} __packed;

/*
 * Structure used to store a range of physical blocks
 */
struct apfs_prange {
	__le64 pr_start_paddr;
	__le64 pr_block_count;
} __packed;

/* Reaper flags */
#define	APFS_NR_BHM_FLAG		0x00000001
#define	APFS_NR_CONTINUE		0x00000002
#define APFS_NR_FLAGS_VALID_MASK	(APFS_NR_BHM_FLAG | APFS_NR_CONTINUE)

/*
 * On-disk reaper structure
 */
struct apfs_nx_reaper_phys {
	struct apfs_obj_phys	nr_o;
	__le64			nr_next_reap_id;
	__le64			nr_completed_id;
	__le64			nr_head;
	__le64			nr_tail;
	__le32			nr_flags;
	__le32			nr_rlcount;
	__le32			nr_type;
	__le32			nr_size;
	__le64			nr_fs_oid;
	__le64			nr_oid;
	__le64			nr_xid;
	__le32			nr_nrle_flags;
	__le32			nr_state_buffer_size;
	u8			nr_state_buffer[];
} __packed;

struct apfs_nx_reap_list_entry {
	__le32	nrle_next;
	__le32	nrle_flags;
	__le32	nrle_type;
	__le32	nrle_size;
	__le64	nrle_fs_oid;
	__le64	nrle_oid;
	__le64	nrle_xid;
} __packed;

struct apfs_nx_reap_list_phys {
	struct apfs_obj_phys		nrl_o;
	__le64				nrl_next;
	__le32				nrl_flags;
	__le32				nrl_max;
	__le32				nrl_count;
	__le32				nrl_first;
	__le32				nrl_last;
	__le32				nrl_free;
	struct apfs_nx_reap_list_entry	nrl_entries[];
} __packed;

/* EFI constants */
#define APFS_NX_EFI_JUMPSTART_MAGIC	0x5244534A
#define APFS_NX_EFI_JUMPSTART_VERSION	1

/*
 * Information about the embedded EFI driver
 */
struct apfs_nx_efi_jumpstart {
	struct apfs_obj_phys	nej_o;
	__le32			nej_magic;
	__le32			nej_version;
	__le32			nej_efi_file_len;
	__le32			nej_num_extents;
	__le64			nej_reserved[16];
	struct apfs_prange	nej_rec_extents[];
} __packed;

/* Main container */

/* Container constants */
#define APFS_NX_MAGIC				0x4253584E
#define APFS_NX_BLOCK_NUM			0
#define APFS_NX_MAX_FILE_SYSTEMS		100

#define APFS_NX_EPH_INFO_COUNT			4
#define APFS_NX_EPH_MIN_BLOCK_COUNT		8
#define APFS_NX_MAX_FILE_SYSTEM_EPH_STRUCTS	4
#define APFS_NX_TX_MIN_CHECKPOINT_COUNT		4
#define APFS_NX_EPH_INFO_VERSION_1		1

/* Container flags */
#define APFS_NX_RESERVED_1			0x00000001LL
#define APFS_NX_RESERVED_2			0x00000002LL
#define APFS_NX_CRYPTO_SW			0x00000004LL
#define APFS_NX_FLAGS_VALID_MASK		(APFS_NX_RESERVED_1 \
						| APFS_NX_RESERVED_2 \
						| APFS_NX_CRYPTO_SW)

/* Optional container feature flags */
#define APFS_NX_FEATURE_DEFRAG			0x0000000000000001ULL
#define APFS_NX_FEATURE_LCFD			0x0000000000000002ULL
#define APFS_NX_SUPPORTED_FEATURES_MASK		(APFS_NX_FEATURE_DEFRAG | \
						APFS_NX_FEATURE_LCFD)

/* Read-only compatible container feature flags */
#define APFS_NX_SUPPORTED_ROCOMPAT_MASK		(0x0ULL)

/* Incompatible container feature flags */
#define APFS_NX_INCOMPAT_VERSION1		0x0000000000000001ULL
#define APFS_NX_INCOMPAT_VERSION2		0x0000000000000002ULL
#define APFS_NX_INCOMPAT_FUSION			0x0000000000000100ULL
#define APFS_NX_SUPPORTED_INCOMPAT_MASK		(APFS_NX_INCOMPAT_VERSION2 \
						| APFS_NX_INCOMPAT_FUSION)

/* Block and container sizes */
#define APFS_NX_MINIMUM_BLOCK_SIZE		4096
#define APFS_NX_DEFAULT_BLOCK_SIZE		4096
#define APFS_NX_MAXIMUM_BLOCK_SIZE		65536
#define APFS_NX_MINIMUM_CONTAINER_SIZE		1048576

/* Indexes into a container superblock's array of counters */
enum {
	APFS_NX_CNTR_OBJ_CKSUM_SET	= 0,
	APFS_NX_CNTR_OBJ_CKSUM_FAIL	= 1,

	APFS_NX_NUM_COUNTERS		= 32
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0)
#define UUID_SIZE 16
#endif

/*
 * On-disk representation of the container superblock
 */
struct apfs_nx_superblock {
/*00*/	struct apfs_obj_phys nx_o;
/*20*/	__le32 nx_magic;
	__le32 nx_block_size;
	__le64 nx_block_count;

/*30*/	__le64 nx_features;
	__le64 nx_readonly_compatible_features;
	__le64 nx_incompatible_features;

/*48*/	char nx_uuid[UUID_SIZE];

/*58*/	__le64 nx_next_oid;
	__le64 nx_next_xid;

/*68*/	__le32 nx_xp_desc_blocks;
	__le32 nx_xp_data_blocks;
/*70*/	__le64 nx_xp_desc_base;
	__le64 nx_xp_data_base;
	__le32 nx_xp_desc_next;
	__le32 nx_xp_data_next;
/*88*/	__le32 nx_xp_desc_index;
	__le32 nx_xp_desc_len;
	__le32 nx_xp_data_index;
	__le32 nx_xp_data_len;

/*98*/	__le64 nx_spaceman_oid;
	__le64 nx_omap_oid;
	__le64 nx_reaper_oid;

/*B0*/	__le32 nx_test_type;

	__le32 nx_max_file_systems;
/*B8*/	__le64 nx_fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
/*3D8*/	__le64 nx_counters[APFS_NX_NUM_COUNTERS];
/*4D8*/	struct apfs_prange nx_blocked_out_prange;
	__le64 nx_evict_mapping_tree_oid;
/*4F0*/	__le64 nx_flags;
	__le64 nx_efi_jumpstart;
/*500*/	char nx_fusion_uuid[UUID_SIZE];
	struct apfs_prange nx_keylocker;
/*520*/	__le64 nx_ephemeral_info[APFS_NX_EPH_INFO_COUNT];

/*540*/	__le64 nx_test_oid;

	__le64 nx_fusion_mt_oid;
/*550*/	__le64 nx_fusion_wbc_oid;
	struct apfs_prange nx_fusion_wbc;

	__le64 nx_newest_mounted_version;

/*570*/	struct apfs_prange nx_mkb_locker;
} __packed;

/*
 * A mapping from an ephemeral object id to its physical address
 */
struct apfs_checkpoint_mapping {
	__le32 cpm_type;
	__le32 cpm_subtype;
	__le32 cpm_size;
	__le32 cpm_pad;
	__le64 cpm_fs_oid;
	__le64 cpm_oid;
	__le64 cpm_paddr;
} __packed;

/* Checkpoint flags */
#define	APFS_CHECKPOINT_MAP_LAST	0x00000001

/*
 * A checkpoint-mapping block
 */
struct apfs_checkpoint_map_phys {
	struct apfs_obj_phys		cpm_o;
	__le32				cpm_flags;
	__le32				cpm_count;
	struct apfs_checkpoint_mapping	cpm_map[];
} __packed;

/* Volume */

/* Volume constants */
#define APFS_MAGIC				0x42535041

#define APFS_MAX_HIST				8
#define APFS_VOLNAME_LEN			256

/* Volume flags */
#define APFS_FS_UNENCRYPTED			0x00000001LL
#define APFS_FS_EFFACEABLE			0x00000002LL
#define APFS_FS_RESERVED_4			0x00000004LL
#define APFS_FS_ONEKEY				0x00000008LL
#define APFS_FS_SPILLEDOVER			0x00000010LL
#define APFS_FS_RUN_SPILLOVER_CLEANER		0x00000020LL
#define APFS_FS_ALWAYS_CHECK_EXTENTREF		0x00000040LL
#define APFS_FS_FLAGS_VALID_MASK		(APFS_FS_UNENCRYPTED \
					       | APFS_FS_EFFACEABLE \
					       | APFS_FS_RESERVED_4 \
					       | APFS_FS_ONEKEY \
					       | APFS_FS_SPILLEDOVER \
					       | APFS_FS_RUN_SPILLOVER_CLEANER \
					       | APFS_FS_ALWAYS_CHECK_EXTENTREF)

#define APFS_FS_CRYPTOFLAGS			(APFS_FS_UNENCRYPTED \
						| APFS_FS_EFFACEABLE \
						| APFS_FS_ONEKEY)

/* Volume roles */
#define APFS_VOLUME_ENUM_SHIFT		6
#define APFS_VOL_ROLE_NONE		0x0000
#define APFS_VOL_ROLE_SYSTEM		0x0001
#define APFS_VOL_ROLE_USER		0x0002
#define APFS_VOL_ROLE_RECOVERY		0x0004
#define APFS_VOL_ROLE_VM		0x0008
#define APFS_VOL_ROLE_PREBOOT		0x0010
#define APFS_VOL_ROLE_INSTALLER		0x0020
#define APFS_VOL_ROLE_DATA		(1 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_BASEBAND		(2 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_UPDATE		(3 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_XART		(4 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_HARDWARE		(5 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_BACKUP		(6 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_RESERVED_7	(7 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_RESERVED_8	(8 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_ENTERPRISE	(9 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_RESERVED_10	(10 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_PRELOGIN		(11 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLES_VALID_MASK	(APFS_VOL_ROLE_SYSTEM \
					| APFS_VOL_ROLE_USER \
					| APFS_VOL_ROLE_RECOVERY \
					| APFS_VOL_ROLE_VM \
					| APFS_VOL_ROLE_PREBOOT \
					| APFS_VOL_ROLE_INSTALLER \
					| APFS_VOL_ROLE_DATA \
					| APFS_VOL_ROLE_BASEBAND \
					| APFS_VOL_ROLE_UPDATE \
					| APFS_VOL_ROLE_XART \
					| APFS_VOL_ROLE_HARDWARE \
					| APFS_VOL_ROLE_BACKUP \
					| APFS_VOL_ROLE_RESERVED_7 \
					| APFS_VOL_ROLE_RESERVED_8 \
					| APFS_VOL_ROLE_ENTERPRISE \
					| APFS_VOL_ROLE_RESERVED_10 \
					| APFS_VOL_ROLE_PRELOGIN)

/* Optional volume feature flags */
#define APFS_FEATURE_DEFRAG_PRERELEASE		0x00000001LL
#define APFS_FEATURE_HARDLINK_MAP_RECORDS	0x00000002LL
#define APFS_FEATURE_DEFRAG			0x00000004LL
#define APFS_FEATURE_STRICTATIME		0x00000008LL
#define APFS_FEATURE_VOLGRP_SYSTEM_INO_SPACE	0x00000010LL

#define APFS_SUPPORTED_FEATURES_MASK	(APFS_FEATURE_DEFRAG \
					| APFS_FEATURE_DEFRAG_PRERELEASE \
					| APFS_FEATURE_HARDLINK_MAP_RECORDS \
					| APFS_FEATURE_STRICTATIME \
					| APFS_FEATURE_VOLGRP_SYSTEM_INO_SPACE)

/* Read-only compatible volume feature flags */
#define APFS_SUPPORTED_ROCOMPAT_MASK		(0x0ULL)

/* Incompatible volume feature flags */
#define APFS_INCOMPAT_CASE_INSENSITIVE		0x00000001LL
#define APFS_INCOMPAT_DATALESS_SNAPS		0x00000002LL
#define APFS_INCOMPAT_ENC_ROLLED		0x00000004LL
#define APFS_INCOMPAT_NORMALIZATION_INSENSITIVE	0x00000008LL
#define APFS_INCOMPAT_INCOMPLETE_RESTORE	0x00000010LL
#define APFS_INCOMPAT_SEALED_VOLUME		0x00000020LL
#define APFS_INCOMPAT_RESERVED_40		0x00000040LL

#define APFS_SUPPORTED_INCOMPAT_MASK (APFS_INCOMPAT_CASE_INSENSITIVE \
				     | APFS_INCOMPAT_DATALESS_SNAPS \
				     | APFS_INCOMPAT_ENC_ROLLED \
				     | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE \
				     | APFS_INCOMPAT_INCOMPLETE_RESTORE \
				     | APFS_INCOMPAT_SEALED_VOLUME \
				     | APFS_INCOMPAT_RESERVED_40)

#define APFS_MODIFIED_NAMELEN	      32

/*
 * Structure containing information about a program that modified the volume
 */
struct apfs_modified_by {
	u8 id[APFS_MODIFIED_NAMELEN];
	__le64 timestamp;
	__le64 last_xid;
} __packed;

/* Version constants for wrapped meta crypto state */
#define APFS_WMCS_MAJOR_VERSION		5
#define APFS_WMCS_MINOR_VERSION		0

/* Protection classes */
#define APFS_PROTECTION_CLASS_DIR_NONE	0 /* Inherits the directory's default */
#define APFS_PROTECTION_CLASS_A		1
#define APFS_PROTECTION_CLASS_B		2
#define APFS_PROTECTION_CLASS_C		3
#define APFS_PROTECTION_CLASS_D		4 /* No protection */
#define APFS_PROTECTION_CLASS_F		6 /* No protection, nonpersistent key */

/* Encryption identifiers */
#define APFS_CRYPTO_SW_ID		4
#define APFS_CRYPTO_RESERVED_5		5
#define APFS_UNASSIGNED_CRYPTO_ID	(~0ULL)

/*
 * Structure used to store the encryption state
 */
struct apfs_wrapped_meta_crypto_state {
	__le16 major_version;
	__le16 minor_version;
	__le32 cpflags;
	__le32 persistent_class;
	__le32 key_os_version;
	__le16 key_revision;
	__le16 unused;
} __packed;

/*
 * On-disk representation of a volume superblock
 */
struct apfs_superblock {
/*00*/	struct apfs_obj_phys apfs_o;

/*20*/	__le32 apfs_magic;
	__le32 apfs_fs_index;

/*28*/	__le64 apfs_features;
	__le64 apfs_readonly_compatible_features;
	__le64 apfs_incompatible_features;

/*40*/	__le64 apfs_unmount_time;

	__le64 apfs_fs_reserve_block_count;
	__le64 apfs_fs_quota_block_count;
	__le64 apfs_fs_alloc_count;

/*60*/	struct apfs_wrapped_meta_crypto_state apfs_meta_crypto;

/*74*/	__le32 apfs_root_tree_type;
	__le32 apfs_extentref_tree_type;
	__le32 apfs_snap_meta_tree_type;

/*80*/	__le64 apfs_omap_oid;
	__le64 apfs_root_tree_oid;
	__le64 apfs_extentref_tree_oid;
	__le64 apfs_snap_meta_tree_oid;

/*A0*/	__le64 apfs_revert_to_xid;
	__le64 apfs_revert_to_sblock_oid;

/*B0*/	__le64 apfs_next_obj_id;

/*B8*/	__le64 apfs_num_files;
	__le64 apfs_num_directories;
	__le64 apfs_num_symlinks;
	__le64 apfs_num_other_fsobjects;
	__le64 apfs_num_snapshots;

/*E0*/	__le64 apfs_total_blocks_alloced;
	__le64 apfs_total_blocks_freed;

/*F0*/	char apfs_vol_uuid[UUID_SIZE];
/*100*/	__le64 apfs_last_mod_time;

	__le64 apfs_fs_flags;

/*110*/	struct apfs_modified_by apfs_formatted_by;
/*140*/	struct apfs_modified_by apfs_modified_by[APFS_MAX_HIST];

/*2C0*/	u8 apfs_volname[APFS_VOLNAME_LEN];
/*3C0*/	__le32 apfs_next_doc_id;

	__le16 apfs_role;
	__le16 reserved;

/*3C8*/	__le64 apfs_root_to_xid;
	__le64 apfs_er_state_oid;

	__le64 apfs_cloneinfo_id_epoch;
	__le64 apfs_cloneinfo_xid;

	__le64 apfs_snap_meta_ext_oid;

	char apfs_volume_group_id[UUID_SIZE];

	__le64 apfs_integrity_meta_oid;

	__le64 apfs_fext_tree_oid;
	__le32 apfs_fext_tree_type;

	__le32 reserved_type;
	__le64 reserved_oid;
} __packed;

/* Extended attributes constants */
#define APFS_XATTR_MAX_EMBEDDED_SIZE	3804

/* Extended attributes names */
#define APFS_XATTR_NAME_SYMLINK		"com.apple.fs.symlink"
#define APFS_XATTR_NAME_COMPRESSED	"com.apple.decmpfs"
#define APFS_XATTR_NAME_RSRC_FORK	"com.apple.ResourceFork"
#define APFS_XATTR_NAME_SECURITY	"com.apple.system.Security"
#define APFS_XATTR_NAME_FINDER_INFO	"com.apple.FinderInfo"

/* Extended attributes flags */
enum {
	APFS_XATTR_DATA_STREAM		= 0x00000001,
	APFS_XATTR_DATA_EMBEDDED	= 0x00000002,
	APFS_XATTR_FILE_SYSTEM_OWNED	= 0x00000004,
	APFS_XATTR_RESERVED_8		= 0x00000008,
};

#define APFS_XATTR_VALID_FLAGS 0x0000000f

/*
 * Structure of the value of an extended attributes record
 */
struct apfs_xattr_val {
	__le16 flags;
	__le16 xdata_len;
	u8 xdata[0];
} __packed;

/*
 * Structure used to store the data of an extended attributes record
 */
struct apfs_xattr_dstream {
	__le64 xattr_obj_id;
	struct apfs_dstream dstream;
} __packed;

/*
 * Integrity metadata for a sealed volume
 */
struct apfs_integrity_meta_phys {
	struct apfs_obj_phys im_o;

	__le32	im_version;
	__le32	im_flags;
	__le32	im_hash_type;
	__le32	im_root_hash_offset;
	__le64	im_broken_xid;
	__le64	im_reserved[9];
} __packed;

/*
 * Version numbers for the integrity metadata structure
 */
enum {
	APFS_INTEGRITY_META_VERSION_INVALID	= 0,
	APFS_INTEGRITY_META_VERSION_1		= 1,
	APFS_INTEGRITY_META_VERSION_2		= 2,
	APFS_INTEGRITY_META_VERSION_HIGHEST	= APFS_INTEGRITY_META_VERSION_2
};

/* Flags used by integrity metadata */
#define APFS_SEAL_BROKEN (1U << 0)

/*
 * Constants used to identify hash algorithms
 */
enum {
	APFS_HASH_INVALID	= 0,
	APFS_HASH_SHA256	= 0x1,
	APFS_HASH_SHA512_256	= 0x2,
	APFS_HASH_SHA384	= 0x3,
	APFS_HASH_SHA512	= 0x4,

	APFS_HASH_MIN		= APFS_HASH_SHA256,
	APFS_HASH_MAX		= APFS_HASH_SHA512,
	APFS_HASH_DEFAULT	= APFS_HASH_SHA256,
};

#define APFS_HASH_CCSHA256_SIZE		32
#define APFS_HASH_CCSHA512_256_SIZE	32
#define APFS_HASH_CCSHA384_SIZE		48
#define APFS_HASH_CCSHA512_SIZE		64

#define APFS_HASH_MAX_SIZE		64

/*
 * Structure of a key in a fext tree
 */
struct apfs_fext_tree_key {
	__le64 private_id;
	__le64 logical_addr;
} __packed;

/*
 * Structure of a value in a fext tree
 */
struct apfs_fext_tree_val {
	__le64 len_and_flags;
	__le64 phys_block_num;
} __packed;

/*
 * Structure of the key for a file info record
 */
struct apfs_file_info_key {
	struct apfs_key_header hdr;
	__le64 info_and_lba;
} __packed;

#define APFS_FILE_INFO_LBA_MASK		0x00ffffffffffffffULL
#define APFS_FILE_INFO_TYPE_MASK	0xff00000000000000ULL
#define APFS_FILE_INFO_TYPE_SHIFT	56

/*
 * A hash of file data
 */
struct apfs_file_data_hash_val {
	__le16	hashed_len;
	u8	hash_size;
	u8	hash[0];
} __packed;

#define APFS_FILE_INFO_DATA_HASH	1

/*
 * Structure of the value for a file info record
 */
struct apfs_file_info_val {
	union {
		struct apfs_file_data_hash_val dhash;
	};
} __packed;

#define APFS_BTREE_NODE_HASH_SIZE_MAX	64

/*
 * Structure of the value of an index record for a hashed catalog tree
 */
struct apfs_btn_index_node_val {
	__le64	binv_child_oid;
	/*
	 * The reference seems to be wrong about the hash size, at least for
	 * SHA-256. TODO: check what happens with other hash functions.
	 */
	u8	binv_child_hash[APFS_HASH_CCSHA256_SIZE];
} __packed;

/*
 * Compressed file header
 */
struct apfs_compress_hdr {
	__le32 signature;
	__le32 algo;
	__le64 size;
} __packed;

#define APFS_COMPRESS_ZLIB_ATTR		3
#define APFS_COMPRESS_ZLIB_RSRC		4
#define APFS_COMPRESS_LZVN_ATTR		7
#define APFS_COMPRESS_LZVN_RSRC		8
#define APFS_COMPRESS_PLAIN_ATTR	9
#define APFS_COMPRESS_PLAIN_RSRC	10
#define APFS_COMPRESS_LZFSE_ATTR	11
#define APFS_COMPRESS_LZFSE_RSRC	12
#define APFS_COMPRESS_LZBITMAP_ATTR	13
#define APFS_COMPRESS_LZBITMAP_RSRC	14

struct apfs_compress_rsrc_hdr {
	__be32 data_offs;
	__be32 mgmt_offs;
	__be32 data_size;
	__be32 mgmt_size;
} __packed;

#define APFS_COMPRESS_BLOCK		0x10000

struct apfs_compress_rsrc_data {
	__le32 unknown;
	__le32 num;
	struct apfs_compress_rsrc_block {
		__le32 offs;
		__le32 size;
	} __packed block[0];
} __packed;

/*
 * TODO: this is a placeholder, create some snapshots with the official
 * implementation to find the actual limit.
 */
#define APFS_SNAP_MAX_NAMELEN		255

/*
 * Structure of the value of a snapshot metadata record
 */
struct apfs_snap_metadata_val {
	__le64 extentref_tree_oid;
	__le64 sblock_oid;
	__le64 create_time;
	__le64 change_time;
	__le64 inum;
	__le32 extentref_tree_type;
	__le32 flags;
	__le16 name_len;
	u8 name[0];
} __packed;

/*
 * Structure of the value of a snapshot name record
 */
struct apfs_snap_name_val {
	__le64 snap_xid;
} __packed;

/*
 * Structure of the extended snapshot metadata
 */
struct apfs_snap_meta_ext {
	struct apfs_obj_phys sme_o;

	__le32	sme_version;
	__le32	sme_flags;
	__le64	sme_snap_xid;
	char 	sme_uuid[UUID_SIZE];
	__le64	sme_token;
} __packed;

#define APFS_OBJECT_TYPE_KEYBAG	0x6b657973 /* Spells 'syek' */

#define	APFS_VOL_KEYBAG_ENTRY_MAX_SIZE		512
#define APFS_FV_PERSONAL_RECOVERY_KEY_UUID	"EBC6C064-0000-11AA-AA11-00306543ECAC"

/* Keybag entry types */
enum {
	KB_TAG_UNKNOWN			= 0,
	KB_TAG_RESERVED_1		= 1,

	KB_TAG_VOLUME_KEY		= 2,
	KB_TAG_VOLUME_UNLOCK_RECORDS	= 3,
	KB_TAG_VOLUME_PASSPHRASE_HINT	= 4,

	KB_TAG_WRAPPING_M_KEY		= 5,
	KB_TAG_VOLUME_M_KEY		= 6,

	KB_TAG_RESERVED_F8		= 0xF8
};

/*
 * Structure of a single entry in the keybag
 */
struct apfs_keybag_entry {
	char 	ke_uuid[UUID_SIZE];
	__le16	ke_tag;
	__le16	ke_keylen;
	__le32	padding;
	u8	ke_keydata[0];
} __packed;

#define APFS_KEYBAG_VERSION	2

/*
 * Structure of the locker in the keybag
 */
struct apfs_kb_locker {
	__le16				kl_version;
	__le16				kl_nkeys;
	__le32				kl_nbytes;
	__le64				padding;
	struct apfs_keybag_entry	kl_entries[0];
} __packed;

#endif	/* _APFS_RAW_H */
