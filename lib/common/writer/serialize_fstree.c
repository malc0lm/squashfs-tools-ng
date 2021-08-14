/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * serialize_fstree.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static sqfs_inode_generic_t *tree_node_to_inode(tree_node_t *node)
{
	sqfs_inode_generic_t *inode;
	size_t extra = 0;

	if (S_ISLNK(node->mode))
		extra = strlen(node->data.target);

	inode = calloc(1, sizeof(*inode) + extra);
	if (inode == NULL) {
		perror("creating inode");
		return NULL;
	}

	switch (node->mode & S_IFMT) {
	case S_IFSOCK:
		inode->base.type = SQFS_INODE_SOCKET;
		inode->data.ipc.nlink = node->link_count;
		break;
	case S_IFIFO:
		inode->base.type = SQFS_INODE_FIFO;
		inode->data.ipc.nlink = node->link_count;
		break;
	case S_IFLNK:
		inode->base.type = SQFS_INODE_SLINK;
		inode->data.slink.nlink = node->link_count;
		inode->data.slink.target_size = extra;
		memcpy(inode->extra, node->data.target, extra);
		break;
	case S_IFBLK:
		inode->base.type = SQFS_INODE_BDEV;
		inode->data.dev.nlink = node->link_count;
		inode->data.dev.devno = node->data.devno;
		break;
	case S_IFCHR:
		inode->base.type = SQFS_INODE_CDEV;
		inode->data.dev.nlink = node->link_count;
		inode->data.dev.devno = node->data.devno;
		break;
	default:
		assert(0);
	}

	return inode;
}

static sqfs_inode_generic_t *write_dir_entries(const char *filename,
					       sqfs_dir_writer_t *dirw,
					       tree_node_t *node)
{
	sqfs_u32 xattr, parent_inode;
	sqfs_inode_generic_t *inode;
	tree_node_t *it, *tgt;
	int ret;
	// 初始化 dir table writer
	// 也就是对每一个type为dir的都要begin一下这个 dir writer，每个目录单独记录状态，那么这个状态写完就持久化到序列化文件里了？？？ 
	ret = sqfs_dir_writer_begin(dirw, 0);
	if (ret)
		goto fail;

	// 遍历所有当前inode的子节点
	for (it = node->data.dir.children; it != NULL; it = it->next) {
		// 如果是hardlink，把目标node赋值给 tgt，
		if (it->mode == FSTREE_MODE_HARD_LINK_RESOLVED) {
			tgt = it->data.target_node;
		} else {
			tgt = it;
		}
		// 将tree node 转化为entry 写入到writer的临时链表 便于后面write到dm
		// 现在问题在 inode_ref是在什么时候计算出来的，为什么treenode里就自带了？？？
		ret = sqfs_dir_writer_add_entry(dirw, it->name, tgt->inode_num,
						tgt->inode_ref, tgt->mode);
		if (ret)
			goto fail;
	}
	// writer 结束，这里实现了很多逻辑，writer_end 主要就是把所有entry写入dm，包括生成的header和entry中不定长的name
	ret = sqfs_dir_writer_end(dirw);
	if (ret)
		goto fail;

	xattr = node->xattr_idx;
	parent_inode = (node->parent == NULL) ? 0 : node->parent->inode_num;
	// 创建 dir 本身的inode，这里涉及到计算很多dir index的数据，但是apparate应该不需要
	// 只需要生成一个带xattr 且不需要index的ext dir inode
	inode = sqfs_dir_writer_create_inode(dirw, 0, xattr, parent_inode);
	if (inode == NULL) {
		ret = SQFS_ERROR_ALLOC;
		goto fail;
	}

	if (inode->base.type == SQFS_INODE_DIR) {
		inode->data.dir.nlink = node->link_count;
	} else {
		inode->data.dir_ext.nlink = node->link_count;
	}

	return inode;
fail:
	sqfs_perror(filename, "recoding directory entries", ret);
	return NULL;
}

static int serialize_tree_node(const char *filename, sqfs_writer_t *wr,
			       tree_node_t *n)
{
	sqfs_inode_generic_t *inode;
	sqfs_u32 offset;
	sqfs_u64 block;
	int ret;
	// 现在依然有inode ref问题，什么时候开始计算的
	// 	函数尾部计算，n->inode_ref = (block << 16) | offset;
	// 所以这个树一定是 自底向上计算，先算叶子节点，也就是我写entry的时候就已经提前知道了当前这个目录的inode已经有多个
	if (S_ISDIR(n->mode)) {
		// 当n 这个输入fs inode 是个目录，那么直接 序列化 dir entry
		// 也就是这个原始fs inode数组排列里 依然包含了子节点指针信息
		// 先把children中变成entry 写入writer的临时entry链表中
		// 再把entry完整写入dm中
		inode = write_dir_entries(filename, wr->dirwr, n);
		ret = SQFS_ERROR_INTERNAL;
	} else if (S_ISREG(n->mode)) {
		//如果是普通文件，这里似乎强转成了sqfs inode，我们可以直接使用现成的
		inode = n->data.file.user_ptr;
		n->data.file.user_ptr = NULL;
		ret = SQFS_ERROR_INTERNAL;
		// 这里判断了 是不是变成扩展普通文件类型，当inode本身就是从sqfs里提取的话，是否还需要？？？
		// 可能需要，理想的方法是最后的方式是fstree合并， 但是还记录着sqfs inode 数据块地址偏移

		// 硬链接这里需要仔细思考一下和软连接（在merge 算法里）
		// dump的时候暂时忽略
		if (inode->base.type == SQFS_INODE_FILE && n->link_count > 1) {
			sqfs_inode_make_extended(inode);
			inode->data.file_ext.nlink = n->link_count;
		} else {
			inode->data.file_ext.nlink = n->link_count;
		}
	} else {
		// 除了 dir 和 file 其他类型直接强转成 sqfs inode
		// 在apparate中直接用这个inode就行
		inode = tree_node_to_inode(n);
		ret = SQFS_ERROR_ALLOC;
	}

	if (inode == NULL)
		return ret;

	// 这下面三行都不需要已经有sqfs inode
	inode->base.mode = n->mode;
	inode->base.mod_time = n->mod_time;
	inode->base.inode_number = n->inode_num;

	// 从在内存设置xattr index，理论上做inode表的时候 就要考虑xattr 表，id table表，cache table表，blob table表，fragment table表
	sqfs_inode_set_xattr_index(inode, n->xattr_idx);

	ret = sqfs_id_table_id_to_index(wr->idtbl, n->uid,
					&inode->base.uid_idx);
	if (ret)
		goto out;

	ret = sqfs_id_table_id_to_index(wr->idtbl, n->gid,
					&inode->base.gid_idx);
	if (ret)
		goto out;
	// 给当前metadata block 和 offset 地址赋值，用以计算inode_ref
	sqfs_meta_writer_get_position(wr->im, &block, &offset);
	/* inode_ref = SquashFS inode refernce number. 32 bit offset of the meta data
	   block start (relative to inode table start), shifted left by 16
	   and ored with a 13 bit offset into the uncompressed meta data block.

	   ！！！Generated on the fly when writing inodes. */
	// 每次处理一个inode，就要在输入的原始inode计算inode_ref，
	// answer: 用以给后面dir table entry提供数据，达到联动的效果，所以一定是自底向上遍历
	// 计算方法  block左移 16位 取或 offset 为什么？？？
	// 也就是随着数组的逐个处理，inode_ref 是根据当前的im block 和offset计算出来的
	n->inode_ref = (block << 16) | offset;
	// 在 inode table block 分配器中写入一个inode
	ret = sqfs_meta_writer_write_inode(wr->im, inode);
out:
	free(inode);
	return ret;
}

// 这个filename是输出sqfs的文件名
int sqfs_serialize_fstree(const char *filename, sqfs_writer_t *wr)
{
	size_t i;
	int ret;
	// writer sqfs文件里要记录一个全局大小，写出已经write了多少数据，好记录superblock的各类start
	wr->super.inode_table_start = wr->outfile->get_size(wr->outfile);

	// 对每个inode数组，挨个遍历，逐个序列化inode,这个inode是fstree inode，还需要转换成sqfs inode，这里是在内存序列化？？？
	for (i = 0; i < wr->fs.unique_inode_count; ++i) {
		// 逐个 inode 序列化 预计 inode table 和dir table 一起生成在内存了
		ret = serialize_tree_node(filename, wr, wr->fs.inodes[i]);
		if (ret)
			goto out;
	}

	//  im dm 是metadata block分配器  im是inodetable的block  dm是dirtable的block，这个两个mb(meta block)
	// 分配器状态要一直保持到写结束，最后直接flush block ，这个inode table meta会直接写磁盘
	ret = sqfs_meta_writer_flush(wr->im);
	if (ret)
		goto out;

	// 这个dir flush 只会先写内存布局
	ret = sqfs_meta_writer_flush(wr->dm);
	if (ret)
		goto out;

	wr->super.root_inode_ref = wr->fs.root->inode_ref;
	wr->super.directory_table_start = wr->outfile->get_size(wr->outfile);
	// 这里又用 dm flush了 dir table ？？？
	// answer: 因为dm 是一直保存在内存里，没有落盘到文件，需要等inode写完后再一次性落盘dir table metablock
	ret = sqfs_meta_write_write_to_file(wr->dm);
	if (ret)
		goto out;

	ret = 0;
out:
	if (ret)
		sqfs_perror(filename, "storing filesystem tree", ret);
	return ret;
}
