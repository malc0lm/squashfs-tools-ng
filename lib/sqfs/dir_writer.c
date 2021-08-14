/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * dir_writer.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include "sqfs/meta_writer.h"
#include "sqfs/dir_writer.h"
#include "sqfs/super.h"
#include "sqfs/table.h"
#include "sqfs/inode.h"
#include "sqfs/error.h"
#include "sqfs/block.h"
#include "sqfs/dir.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

typedef struct dir_entry_t {
	struct dir_entry_t *next;
	sqfs_u64 inode_ref;
	sqfs_u32 inode_num;
	sqfs_u16 type;
	size_t name_len;
	char name[];
} dir_entry_t;

typedef struct index_ent_t {
	struct index_ent_t *next;
	dir_entry_t *ent;
	sqfs_u64 block;
	sqfs_u32 index;
} index_ent_t;

struct sqfs_dir_writer_t {
	sqfs_object_t base;
	// sqfs dir entry 数组
	dir_entry_t *list;
	// sqfs dir entry 数组尾节点
	dir_entry_t *list_end;


	// index entry: A list of directory index entries for faster lookup in the directory table
	// it will index dir entry
	index_ent_t *idx;
	// index entry 数组尾节点
	index_ent_t *idx_end;
	// 目录索引位置？？？
	sqfs_u64 dir_ref;
	// 目录大小
	size_t dir_size;
	// entry总数
	size_t ent_count;
	// dir table metablock 分配器
	sqfs_meta_writer_t *dm;

	// apparate默认不需要export table
	sqfs_u64 *export_tbl;
	size_t export_tbl_max;
	size_t export_tbl_count;
};

static int get_type(sqfs_u16 mode)
{
	switch (mode & S_IFMT) {
	case S_IFSOCK: return SQFS_INODE_SOCKET;
	case S_IFIFO:  return SQFS_INODE_FIFO;
	case S_IFLNK:  return SQFS_INODE_SLINK;
	case S_IFBLK:  return SQFS_INODE_BDEV;
	case S_IFCHR:  return SQFS_INODE_CDEV;
	case S_IFDIR:  return SQFS_INODE_DIR;
	case S_IFREG:  return SQFS_INODE_FILE;
	}

	return SQFS_ERROR_UNSUPPORTED;
}

static void writer_reset(sqfs_dir_writer_t *writer)
{
	dir_entry_t *ent;
	index_ent_t *idx;

	// writer reset逻辑，释放所有idx 链表
	while (writer->idx != NULL) {
		idx = writer->idx;
		writer->idx = idx->next;
		free(idx);
	}
	// writer reset逻辑，释放所有ent 链表
	while (writer->list != NULL) {
		ent = writer->list;
		writer->list = ent->next;
		free(ent);
	}

	writer->list_end = NULL;
	writer->idx_end = NULL;
	writer->dir_ref = 0;
	writer->dir_size = 0;
	writer->ent_count = 0;
}

static int add_export_table_entry(sqfs_dir_writer_t *writer,
				  sqfs_u32 inum, sqfs_u64 iref)
{
	size_t i, new_max;
	sqfs_u64 *new;

	if (writer->export_tbl == NULL)
		return 0;

	if (inum < 1)
		return SQFS_ERROR_ARG_INVALID;

	new_max = writer->export_tbl_max;

	while ((inum - 1) >= new_max) {
		if (SZ_MUL_OV(new_max, 2, &new_max))
			return SQFS_ERROR_ALLOC;
	}

	if (new_max > writer->export_tbl_max) {
		if (SZ_MUL_OV(new_max, sizeof(writer->export_tbl[0]), &new_max))
			return SQFS_ERROR_ALLOC;

		new = realloc(writer->export_tbl, new_max);
		if (new == NULL)
			return SQFS_ERROR_ALLOC;

		new_max /= sizeof(writer->export_tbl[0]);

		for (i = writer->export_tbl_max; i < new_max; ++i)
			new[i] = 0xFFFFFFFFFFFFFFFFUL;

		writer->export_tbl = new;
		writer->export_tbl_max = new_max;
	}

	writer->export_tbl[inum - 1] = iref;

	if ((inum - 1) >= writer->export_tbl_count)
		writer->export_tbl_count = inum;

	return 0;
}

static void dir_writer_destroy(sqfs_object_t *obj)
{
	sqfs_dir_writer_t *writer = (sqfs_dir_writer_t *)obj;

	writer_reset(writer);
	free(writer->export_tbl);
	free(writer);
}

sqfs_dir_writer_t *sqfs_dir_writer_create(sqfs_meta_writer_t *dm,
					  sqfs_u32 flags)
{
	sqfs_dir_writer_t *writer;

	if (flags & ~SQFS_DIR_WRITER_CREATE_ALL_FLAGS)
		return NULL;

	writer = calloc(1, sizeof(*writer));
	if (writer == NULL)
		return NULL;

	if (flags & SQFS_DIR_WRITER_CREATE_EXPORT_TABLE) {
		writer->export_tbl_max = 512;

		writer->export_tbl = calloc(sizeof(writer->export_tbl[0]),
					    writer->export_tbl_max);
		if (writer->export_tbl == NULL) {
			free(writer);
			return NULL;
		}

		memset(writer->export_tbl, 0xFF,
		       sizeof(writer->export_tbl[0]) * writer->export_tbl_max);
	}

	((sqfs_object_t *)writer)->destroy = dir_writer_destroy;
	writer->dm = dm;
	return writer;
}

int sqfs_dir_writer_begin(sqfs_dir_writer_t *writer, sqfs_u32 flags)
{
	sqfs_u32 offset;
	sqfs_u64 block;

	if (flags != 0)
		return SQFS_ERROR_UNSUPPORTED;
	// writer里所有链表和数据重置 
	writer_reset(writer);
	// 从全局一致的dir metablock 分配器里获取当前的block 和 offset
	sqfs_meta_writer_get_position(writer->dm, &block, &offset);
	// 设置 writer的dir_ref
	writer->dir_ref = (block << 16) | offset;
	return 0;
}

int sqfs_dir_writer_add_entry(sqfs_dir_writer_t *writer, const char *name,
			      sqfs_u32 inode_num, sqfs_u64 inode_ref,
			      sqfs_u16 mode)
{
	dir_entry_t *ent;
	int type, err;
	// 普通类型，转换为sqfs类型，这里先默认都是非ext类型
	type = get_type(mode);
	if (type < 0)
		return type;

	if (name[0] == '\0' || inode_num < 1)
		return SQFS_ERROR_ARG_INVALID;
	// 没有export  table 跳过
	err = add_export_table_entry(writer, inode_num, inode_ref);
	if (err)
		return err;
	// 分配了一个 完整entry需要的空间（entry有name 所以不定长）
	ent = alloc_flex(sizeof(*ent), 1, strlen(name));
	if (ent == NULL)
		return SQFS_ERROR_ALLOC;

	// 给entry所有字段复制 包括不定长name
	ent->inode_ref = inode_ref;
	ent->inode_num = inode_num;
	ent->type = type;
	ent->name_len = strlen(name);
	memcpy(ent->name, name, ent->name_len);

	// 加入到链表（这里apparate可能会考虑用vec实现）
	if (writer->list_end == NULL) {
		writer->list = writer->list_end = ent;
	} else {
		writer->list_end->next = ent;
		writer->list_end = ent;
	}
	// 长度ent_count自增1
	writer->ent_count += 1;
	return 0;
}

// 应该是当前的metadata block 还能放后面多少个entry，因为entry不定长里面有name，必须逐个比较
static size_t get_conseq_entry_count(sqfs_u32 offset, dir_entry_t *head)
{
	size_t size, count = 0;
	dir_entry_t *it;
	sqfs_s32 diff;
	// 在某个block的offset+12字节的dir header 取余 8k
	// 那这个size 在8192-12以内 肯定是取余完还是自己，在8192-12 - 8192以内取余完 还剩很小 12字节以内
	size = (offset + sizeof(sqfs_dir_header_t)) % SQFS_META_BLOCK_SIZE;

	// 开始对每个entry遍历
	for (it = head; it != NULL; it = it->next) {
		// inode_ref 什么含义？？？ 右移16位 和 头对比有什么作用？？？
		if ((it->inode_ref >> 16) != (head->inode_ref >> 16))
			break;
		// 计算当前和头结点inode 差异
		diff = it->inode_num - head->inode_num;
		// 超过32767 终止
		if (diff > 32767 || diff < -32767)
			break;
		// size就会继续加上 当前entry的长度。
		size += sizeof(sqfs_dir_entry_t) + it->name_len;

		// 如果当有了count 且size超过了 8k 那就会break
		if (count > 0 && size > SQFS_META_BLOCK_SIZE)
			break;
		// 这里 加完size count自增1
		count += 1;
		// count 超过256 也会break，单个dir header只能有256个entry
		if (count == SQFS_MAX_DIR_ENT)
			break;
	}

	return count;
}

static int add_header(sqfs_dir_writer_t *writer, size_t count,
		      dir_entry_t *ref, sqfs_u64 block)
{
	sqfs_dir_header_t hdr;
	index_ent_t *idx;
	int err;
	// 为什么count - 1？
	hdr.count = htole32(count - 1);
	// 赋值起始metablock
	hdr.start_block = htole32(ref->inode_ref >> 16);
	// dir首个entry的inode_number
	hdr.inode_number = htole32(ref->inode_num);
	// 把header写入内存的metablock
	err = sqfs_meta_writer_append(writer->dm, &hdr, sizeof(hdr));
	if (err)
		return err;
	// 这里开始 有了dir index 操作，但是apparate应该用不到dir index 这个只是加速索引
	idx = calloc(1, sizeof(*idx));
	if (idx == NULL)
		return SQFS_ERROR_ALLOC;

	idx->ent = ref;
	idx->block = block;
	// index 是This stores a byte offset from the first directory header to the current header
	// 即 第一个header 到当前的header有多少字节偏移
	idx->index = writer->dir_size;

	if (writer->idx_end == NULL) {
		writer->idx = writer->idx_end = idx;
	} else {
		writer->idx_end->next = idx;
		writer->idx_end = idx;
	}

	writer->dir_size += sizeof(hdr);
	return 0;
}
// writer_end 就是把所有entry写入dm，包括生成的header和entry中不定长的name
// 如果跨metablock或者单个metablock里太多了，那就新生成header
int sqfs_dir_writer_end(sqfs_dir_writer_t *writer)
{
	dir_entry_t *it, *first;
	sqfs_dir_entry_t ent;
	sqfs_u16 *diff_u16;
	size_t i, count;
	sqfs_u32 offset;
	sqfs_u64 block;
	int err;
	// 这里之所有没有逐个遍历，应该是一次性放完 count个，而不是逐个放置。所以因该是 it=it.next(count次)
	// 实际上这里只有全部entry，header是根据需要自己生成的
	for (it = writer->list; it != NULL; ) {
		//  依然是找到现在dm 已分配后的block offset
		// 这个block 和offset就是 已经是第几个block了，在这个block里用了多少了从哪起始
		sqfs_meta_writer_get_position(writer->dm, &block, &offset);
		// 计算当前的metadata block 还能放后面多少个entry，因为entry不定长里面有name，必须逐个比较
		count = get_conseq_entry_count(offset, it);
		// 添加dir header 传入首个entry，count，metablock
		err = add_header(writer, count, it, block);
		if (err)
			return err;
		// 当前这个header下的first entry
		first = it;
		// 这里开始向header里写入最大的entry
		for (i = 0; i < count; ++i) {
			// 每个entry的offset是指向inode table数据
			ent.offset = htole16(it->inode_ref & 0x0000FFFF);
			ent.inode_diff = it->inode_num - first->inode_num;
			ent.type = htole16(it->type);
			ent.size = htole16(it->name_len - 1);
			// 给这个diff_16赋值的意义是？
			diff_u16 = (sqfs_u16 *)&ent.inode_diff;
			*diff_u16 = htole16(*diff_u16);


			// 加个entry头
			err = sqfs_meta_writer_append(writer->dm, &ent,
						      sizeof(ent));
			if (err)
				return err;
			// 加个entry中的name（不定长）
			err = sqfs_meta_writer_append(writer->dm, it->name,
						      it->name_len);
			if (err)
				return err;
			// dir_dize 就是累加所有entry 和header的uncompress长度
			writer->dir_size += sizeof(ent) + it->name_len;
			it = it->next;
		}
	}

	return 0;
}

size_t sqfs_dir_writer_get_size(const sqfs_dir_writer_t *writer)
{
	return writer->dir_size;
}

sqfs_u64 sqfs_dir_writer_get_dir_reference(const sqfs_dir_writer_t *writer)
{
	return writer->dir_ref;
}

size_t sqfs_dir_writer_get_index_size(const sqfs_dir_writer_t *writer)
{
	size_t index_size = 0;
	index_ent_t *idx;

	for (idx = writer->idx; idx != NULL; idx = idx->next)
		index_size += sizeof(sqfs_dir_index_t) + idx->ent->name_len;

	return index_size;
}

size_t sqfs_dir_writer_get_entry_count(const sqfs_dir_writer_t *writer)
{
	return writer->ent_count;
}

sqfs_inode_generic_t
*sqfs_dir_writer_create_inode(const sqfs_dir_writer_t *writer,
			      size_t hlinks, sqfs_u32 xattr,
			      sqfs_u32 parent_ino)
{
	sqfs_inode_generic_t *inode;
	sqfs_dir_index_t ent;
	sqfs_u64 start_block;
	sqfs_u16 block_offset;
	size_t index_size;
	index_ent_t *idx;
	sqfs_u8 *ptr;

	index_size = 0;

	for (idx = writer->idx; idx != NULL; idx = idx->next)
		index_size += sizeof(ent) + idx->ent->name_len;

	inode = alloc_flex(sizeof(*inode), 1, index_size);
	if (inode == NULL)
		return NULL;

	inode->payload_bytes_available = index_size;
	// writer->dir_ref 只有在每个dir writer 初始化的时候从dm分配器中根据block和offset计算出来
	start_block = writer->dir_ref >> 16;
	block_offset = writer->dir_ref & 0xFFFF;

	// apparate 这里需要做的是 虽然是扩展dir ext dir，但是不去计算inode index
	// 作为TODO项，性能优化
	if (xattr != 0xFFFFFFFF || start_block > 0xFFFFFFFFUL ||
	    writer->dir_size > 0xFFFF) {
		inode->base.type = SQFS_INODE_EXT_DIR;
	} else {
		inode->base.type = SQFS_INODE_DIR;
	}

	if (inode->base.type == SQFS_INODE_DIR) {
		inode->data.dir.start_block = start_block;
		inode->data.dir.nlink = writer->ent_count + hlinks + 2;
		inode->data.dir.size = writer->dir_size;
		inode->data.dir.offset = block_offset;
		inode->data.dir.parent_inode = parent_ino;
	} else {
		inode->data.dir_ext.nlink = writer->ent_count + hlinks + 2;
		inode->data.dir_ext.size = writer->dir_size;
		inode->data.dir_ext.start_block = start_block;
		inode->data.dir_ext.parent_inode = parent_ino;
		inode->data.dir_ext.offset = block_offset;
		inode->data.dir_ext.xattr_idx = xattr;
		inode->data.dir_ext.inodex_count = 0;
		inode->payload_bytes_used = 0;

		for (idx = writer->idx; idx != NULL; idx = idx->next) {
			memset(&ent, 0, sizeof(ent));
			ent.start_block = idx->block;
			ent.index = idx->index;
			ent.size = idx->ent->name_len - 1;

			ptr = (sqfs_u8 *)inode->extra +
				inode->payload_bytes_used;
			memcpy(ptr, &ent, sizeof(ent));
			memcpy(ptr + sizeof(ent), idx->ent->name,
			       idx->ent->name_len);

			inode->data.dir_ext.inodex_count += 1;
			inode->payload_bytes_used += sizeof(ent);
			inode->payload_bytes_used += idx->ent->name_len;
		}
	}

	return inode;
}

int sqfs_dir_writer_write_export_table(sqfs_dir_writer_t *writer,
				       sqfs_file_t *file,
				       sqfs_compressor_t *cmp,
				       sqfs_u32 root_inode_num,
				       sqfs_u64 root_inode_ref,
				       sqfs_super_t *super)
{
	sqfs_u64 start;
	size_t size;
	int ret;

	ret = add_export_table_entry(writer, root_inode_num, root_inode_ref);
	if (ret)
		return 0;

	if (writer->export_tbl_count == 0)
		return 0;

	size = sizeof(writer->export_tbl[0]) * writer->export_tbl_count;

	ret = sqfs_write_table(file, cmp, writer->export_tbl, size, &start);
	if (ret)
		return ret;

	super->export_table_start = start;
	super->flags |= SQFS_FLAG_EXPORTABLE;
	return 0;
}
