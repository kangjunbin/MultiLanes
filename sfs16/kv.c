/** 
 * Copyright (C) 2013-2016 Junbin Kang. All rights reserved.
 */


#include <linux/dcache.h>
#include <linux/rculist.h>
#include <linux/list.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/security.h>
#include <linux/xattr.h>

#include "kv.h"
int reset(struct dentry *dentry, const char *key)
{
	int err;
	int pointer[4] = {-1, -1, -1, -1};
	char buf[20];
	
	sprintf(buf, "%d:%d:%d:%d", pointer[0], pointer[1], pointer[2], pointer[3]);
	err = vfs_setxattr(dentry, key, buf, sizeof(buf), 0);
	if(unlikely(err)){
		printk(KERN_INFO "set xattr failed\n");
	}
	
	return err;
}

int modify(struct dentry *dentry, const char *key, const int value, int sequence)
{

	int err, ret;
	char buf[20];
	int pointer[4] = {-1, -1, -1, -1};

	ret = generic_getxattr(dentry, key, buf, sizeof(buf));
	if(ret > 0){

		sscanf(buf, "%d:%d:%d:%d", &pointer[0], &pointer[1], &pointer[2], &pointer[3]);
	}

	pointer[sequence] = value;
	sprintf(buf, "%d:%d:%d:%d", pointer[0], pointer[1], pointer[2], pointer[3]);
	err = vfs_setxattr(dentry, key, buf, sizeof(buf), 0);
	if(unlikely(err)){
		printk(KERN_INFO "set xattr failed\n");
	}

	return err;
}
int put(struct dentry *dentry, const char *key, const int value[])
{

	int err;
	char buf[15];

	sprintf(buf, "%d:%d:%d:%d", value[0], value[1], value[2], value[3]);
	err = vfs_setxattr(dentry, key, buf, sizeof(buf), 0);
	if(unlikely(err)){
		printk(KERN_INFO "set xattr failed\n");
	}

	return err;
}
int get(struct dentry *dentry, const char *key, int value[])
{

	int ret;
	char buf[20];
	
	ret = vfs_getxattr(dentry, key, buf, sizeof(buf));
	if(ret > 0){
		sscanf(buf, "%d:%d:%d:%d", &value[0], &value[1], &value[2], &value[3]);
	}
	
	return ret;
}
