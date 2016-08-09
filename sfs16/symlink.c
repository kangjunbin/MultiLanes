/** 
 * Copyright (C) 2013-2016 Junbin Kang. All rights reserved.
 */
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/namei.h>

#include <asm/uaccess.h>

#include "sfs_fs.h"

static int sfs_readlink(struct dentry *dentry, char __user *buf, int bufsize)
{
	int err = -EINVAL;
	struct dentry *replica_dentry;

	replica_dentry = list_first_replica_dentry(dentry);
	if(unlikely(!replica_dentry->d_inode->i_op->readlink))
		goto out;

	err = replica_dentry->d_inode->i_op->readlink(replica_dentry, buf, bufsize);
out:
	return err;
}

static void *sfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	int err = 0;
	mm_segment_t old_fs;
	union {
		char *k;
		char __user *u;
	}buf;
	
	buf.k = (void *)__get_free_page(GFP_NOFS);
	if(unlikely(!buf.k)){
		err = -ENOMEM;
		goto out;
	}
	
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = sfs_readlink(dentry, buf.u, PATH_MAX);
	set_fs(old_fs);
	
	if(err >= 0){
		buf.k[err] = 0;
		nd_set_link(nd, buf.k);
		return NULL;
	}
out:
	return ERR_PTR(err);
}

static void sfs_put_link(struct dentry *dentry, struct nameidata *nd,
	void *cookie)
{
	char *p;

	p = nd_get_link(nd);
	if(!IS_ERR_OR_NULL(p))
		free_page((unsigned long)p);
}

const struct inode_operations sfs_symlink_inode_operations = {
	.setattr	= sfs_setattr,
	.getattr	= sfs_getattr,

	.readlink	= sfs_readlink,
	.follow_link	= sfs_follow_link,
	.put_link	= sfs_put_link,
};
