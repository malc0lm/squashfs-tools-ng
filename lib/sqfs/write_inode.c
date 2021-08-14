/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * write_inode.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include "sqfs/meta_writer.h"
#include "sqfs/error.h"
#include "sqfs/inode.h"
#include "sqfs/dir.h"
#include "compat.h"

#include <string.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(__WINDOWS__)
#	include <malloc.h>
#	ifdef _MSC_VER
#		define alloca _alloca
#	endif
#elif defined(HAVE_ALLOCA_H)
#	include <alloca.h>
#endif

static int write_block_sizes(sqfs_meta_writer_t *ir,
			     const sqfs_inode_generic_t *n)
{
	// 为何不能直接写入一个 extra[]数据，而要变成size
	// 是因为要要把数据全部转为little endian htole32 
	sqfs_u32 *sizes;
	size_t i;

	if (n->payload_bytes_used < sizeof(sizes[0]))
		return 0;

	if ((n->payload_bytes_used % sizeof(sizes[0])) != 0)
		return SQFS_ERROR_CORRUPTED;

	sizes = alloca(n->payload_bytes_used);

	for (i = 0; i < (n->payload_bytes_used / sizeof(sizes[0])); ++i)
		sizes[i] = htole32(n->extra[i]);

	return sqfs_meta_writer_append(ir, sizes, n->payload_bytes_used);
}

static int write_dir_index(sqfs_meta_writer_t *ir, const sqfs_u8 *data,
			   size_t count)
{
	sqfs_dir_index_t ent;
	size_t len;
	int err;

	while (count > sizeof(ent)) {
		memcpy(&ent, data, sizeof(ent));
		data += sizeof(ent);
		count -= sizeof(ent);
		len = ent.size + 1;

		if (len > count)
			return SQFS_ERROR_CORRUPTED;

		ent.start_block = htole32(ent.start_block);
		ent.index = htole32(ent.index);
		ent.size = htole32(ent.size);

		err = sqfs_meta_writer_append(ir, &ent, sizeof(ent));
		if (err)
			return err;

		err = sqfs_meta_writer_append(ir, data, len);
		if (err)
			return err;

		data += len;
		count -= len;
	}

	return 0;
}

int sqfs_meta_writer_write_inode(sqfs_meta_writer_t *ir,
				 const sqfs_inode_generic_t *n)
{
	sqfs_inode_t base;
	int ret;

	base.type = htole16(n->base.type);
	base.mode = htole16(n->base.mode & ~SQFS_INODE_MODE_MASK);
	base.uid_idx = htole16(n->base.uid_idx);
	base.gid_idx = htole16(n->base.gid_idx);
	base.mod_time = htole32(n->base.mod_time);
	base.inode_number = htole32(n->base.inode_number);
	// 先把定长数据写进meta
	ret = sqfs_meta_writer_append(ir, &base, sizeof(base));
	if (ret)
		return ret;
	// 根据type 生成不同的body然后继续写入meta
	switch (n->base.type) {
	case SQFS_INODE_DIR: {
		sqfs_inode_dir_t dir = {
			.start_block = htole32(n->data.dir.start_block),
			.nlink = htole32(n->data.dir.nlink),
			.size = htole16(n->data.dir.size),
			.offset = htole16(n->data.dir.offset),
			.parent_inode = htole32(n->data.dir.parent_inode),
		};
		return sqfs_meta_writer_append(ir, &dir, sizeof(dir));
	}
	case SQFS_INODE_FILE: {
		sqfs_inode_file_t file = {
			.blocks_start = htole32(n->data.file.blocks_start),
			// 这里需要更新fragment index，因为合并后fragment table要重新生成
			.fragment_index = htole32(n->data.file.fragment_index),
			.fragment_offset =
				htole32(n->data.file.fragment_offset),
			.file_size = htole32(n->data.file.file_size),
		};
		ret = sqfs_meta_writer_append(ir, &file, sizeof(file));
		if (ret)
			return ret;
		// 这里继续写入不定长的 block数组数据，之所以没有直接用extra是因为需要把数组里所有数据转为le
		return write_block_sizes(ir, n);
	}
	case SQFS_INODE_SLINK: {
		sqfs_inode_slink_t slink = {
			.nlink = htole32(n->data.slink.nlink),
			.target_size = htole32(n->data.slink.target_size),
		};
		ret = sqfs_meta_writer_append(ir, &slink, sizeof(slink));
		if (ret)
			return ret;
		return sqfs_meta_writer_append(ir, n->extra,
					       n->data.slink.target_size);
	}
	case SQFS_INODE_BDEV:
	case SQFS_INODE_CDEV: {
		sqfs_inode_dev_t dev = {
			.nlink = htole32(n->data.dev.nlink),
			.devno = htole32(n->data.dev.devno),
		};
		return sqfs_meta_writer_append(ir, &dev, sizeof(dev));
	}
	case SQFS_INODE_FIFO:
	case SQFS_INODE_SOCKET: {
		sqfs_inode_ipc_t ipc = {
			.nlink = htole32(n->data.ipc.nlink),
		};
		return sqfs_meta_writer_append(ir, &ipc, sizeof(ipc));
	}
	case SQFS_INODE_EXT_DIR: {
		sqfs_inode_dir_ext_t dir = {
			.nlink = htole32(n->data.dir_ext.nlink),
			.size = htole32(n->data.dir_ext.size),
			.start_block = htole32(n->data.dir_ext.start_block),
			.parent_inode = htole32(n->data.dir_ext.parent_inode),
			.inodex_count = htole16(n->data.dir_ext.inodex_count),
			.offset = htole16(n->data.dir_ext.offset),
			.xattr_idx = htole32(n->data.dir_ext.xattr_idx),
		};
		ret = sqfs_meta_writer_append(ir, &dir, sizeof(dir));
		if (ret)
			return ret;
		return write_dir_index(ir, (const sqfs_u8 *)n->extra,
				       n->payload_bytes_used);
	}
	case SQFS_INODE_EXT_FILE: {
		sqfs_inode_file_ext_t file = {
			.blocks_start = htole64(n->data.file_ext.blocks_start),
			.file_size = htole64(n->data.file_ext.file_size),
			.sparse = htole64(n->data.file_ext.sparse),
			.nlink = htole32(n->data.file_ext.nlink),
			.fragment_idx = htole32(n->data.file_ext.fragment_idx),
			.fragment_offset =
				htole32(n->data.file_ext.fragment_offset),
			.xattr_idx = htole32(n->data.file_ext.xattr_idx),
		};
		ret = sqfs_meta_writer_append(ir, &file, sizeof(file));
		if (ret)
			return ret;
		return write_block_sizes(ir, n);
	}
	case SQFS_INODE_EXT_SLINK: {
		sqfs_inode_slink_t slink = {
			.nlink = htole32(n->data.slink_ext.nlink),
			.target_size = htole32(n->data.slink_ext.target_size),
		};
		sqfs_u32 xattr = htole32(n->data.slink_ext.xattr_idx);

		ret = sqfs_meta_writer_append(ir, &slink, sizeof(slink));
		if (ret)
			return ret;
		ret = sqfs_meta_writer_append(ir, n->extra,
					      n->data.slink_ext.target_size);
		if (ret)
			return ret;
		return sqfs_meta_writer_append(ir, &xattr, sizeof(xattr));
	}
	case SQFS_INODE_EXT_BDEV:
	case SQFS_INODE_EXT_CDEV: {
		sqfs_inode_dev_ext_t dev = {
			.nlink = htole32(n->data.dev_ext.nlink),
			.devno = htole32(n->data.dev_ext.devno),
			.xattr_idx = htole32(n->data.dev_ext.xattr_idx),
		};
		return sqfs_meta_writer_append(ir, &dev, sizeof(dev));
	}
	case SQFS_INODE_EXT_FIFO:
	case SQFS_INODE_EXT_SOCKET: {
		sqfs_inode_ipc_ext_t ipc = {
			.nlink = htole32(n->data.ipc_ext.nlink),
			.xattr_idx = htole32(n->data.ipc_ext.xattr_idx),
		};
		return sqfs_meta_writer_append(ir, &ipc, sizeof(ipc));
	}
	}

	return SQFS_ERROR_UNSUPPORTED;
}
