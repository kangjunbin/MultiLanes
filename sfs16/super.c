/** 
 * Copyright (C) 2013-2016 Junbin Kang. All rights reserved.
 */

#include <linux/module.h>
#include <linux/parser.h>
#include <linux/exportfs.h>
#include <linux/mount.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <asm/uaccess.h>

#include "sfs_fs.h"

extern const struct dentry_operations sfs_dentry_operations;

unsigned int replica_count;
static struct sfs_sb_info **sbi_array;

struct sfs_sb_info *get_replica_sbi(int replica_id)
{
	if(replica_id > replica_count - 1)
		return NULL;

	return sbi_array[replica_id];
}
unsigned int get_replica_count(void)
{
	return replica_count;
}

static int sfs_drop_inode(struct inode *inode)
{
	int drop;
	
	drop = generic_drop_inode(inode);
	return drop;
}

static int sfs_sync_fs(struct super_block *sb, int wait)
{
	int err = 0, ret;
	struct super_block *replica_sb;
	struct sfs_sb_info *sbi = SFS_S(sb), *replica_sbi;
	
	if(!sbi)
		goto out;
	
	list_for_each_replica(sbi, replica_sbi){
		replica_sb = replica_sbi->s_sb;
		if(replica_sb->s_op->sync_fs){
			ret = replica_sb->s_op->sync_fs(replica_sb, wait);
			if(unlikely(ret && !err))
				err = ret;
		}
	}

	if(unlikely(err))
		printk(KERN_INFO "sync sb err %d\n", err);
out:
	return err;
}

static int sfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	int err = 0, loop;
	struct path path;
	struct dentry *replica_dentry;
	struct sfs_sb_info *replica_sbi;
	u64 blocks, bfree, bavail, files, ffree;

	blocks	= 0;
	bfree	= 0;
	bavail	= 0;
	files	= 0;
	ffree	= 0;
	
	for(loop = 0; loop < replica_count; loop++){
		
		replica_dentry = find_replica_dentry(dentry, loop);
		if(!replica_dentry || !replica_dentry->d_inode){
			continue;
		}

		replica_sbi = get_replica_sbi(loop);

		path.mnt = replica_sbi->s_mnt;
		path.dentry = replica_dentry; 
	
		err = vfs_statfs(&path, buf);

		if(unlikely(err))
			goto out;
		blocks += buf->f_blocks;
		bfree  += buf->f_bfree;
		bavail += buf->f_bavail;
		files  += buf->f_files;
		ffree  += buf->f_ffree;
	}
	
	buf->f_blocks = blocks;
	buf->f_bfree  = bfree;
	buf->f_bavail = bavail;
	buf->f_files  = files;
	buf->f_ffree  = ffree;

out:
	return err;
}
static int sfs_freeze_fs(struct super_block *sb)
{
	int err = 0, ret;
	struct super_block *replica_sb;
	struct sfs_sb_info *sbi = SFS_S(sb), *replica_sbi;
	
	list_for_each_replica(sbi, replica_sbi){
		replica_sb = replica_sbi->s_sb;
		if(replica_sb->s_op->freeze_fs){
			ret = replica_sb->s_op->freeze_fs(replica_sb);
			if(unlikely(ret && !err))
				err = ret;
		}
	}

	return err;
}

static int sfs_unfreeze_fs(struct super_block *sb)
{
	int err = 0, ret;
	struct super_block *replica_sb;
	struct sfs_sb_info *sbi = SFS_S(sb), *replica_sbi;
	
	list_for_each_replica(sbi, replica_sbi){
		replica_sb = replica_sbi->s_sb;
		if(replica_sb->s_op->unfreeze_fs){
			ret = replica_sb->s_op->unfreeze_fs(replica_sb);
			if(unlikely(ret && !err))
				err = ret;
		}
	}

	return err;
}

static int sfs_show_options(struct seq_file *m, struct dentry *dentry)
{

	
	return 0;

}

static const struct super_operations sfs_sops = {
	.drop_inode	= sfs_drop_inode,
//	.show_options	= sfs_show_options,
	.statfs		= sfs_statfs,
	.sync_fs	= sfs_sync_fs,
	.freeze_fs	= sfs_freeze_fs,
	.unfreeze_fs	= sfs_unfreeze_fs,
};
static struct sfs_sb_info *sfs_master_sbi_alloc(struct super_block *sb)
{
	struct sfs_sb_info *sbi;

	sbi = kzalloc(sizeof(struct sfs_sb_info), GFP_KERNEL);
	if(!sbi)
		return ERR_PTR(-ENOMEM);

	sbi->s_sb = sb;
	sb->s_fs_info = sbi;
	INIT_LIST_HEAD(&sbi->u.m.replicas);
	atomic_set(&sbi->u.m.replica_count, 0);
	cpumask_clear(&sbi->cpu_set);

	return sbi;
}

static struct sfs_sb_info *sfs_replica_sbi_alloc(struct super_block *sb)
{
	struct sfs_sb_info *sbi;

	sbi = kzalloc(sizeof(struct sfs_sb_info), GFP_KERNEL);
	if(!sbi)
		return ERR_PTR(-ENOMEM);

	sbi->s_sb = sb;
	INIT_LIST_HEAD(&sbi->u.r.replica_list);
	cpumask_clear(&sbi->cpu_set);

	return sbi;
}

void s_add_replica(struct sfs_sb_info *sfs_master_sbi, struct sfs_sb_info *sfs_replica_sbi,
	int replica_id)
{

	list_add_tail(&sfs_replica_sbi->u.r.replica_list, &sfs_master_sbi->u.m.replicas);
	atomic_inc(&sfs_master_sbi->u.m.replica_count);
	sfs_replica_sbi->s_replica_id = replica_id;
} 

static void copy_super(struct super_block *dest, struct super_block *src)
{
	dest->s_maxbytes = src->s_maxbytes;
	dest->s_flags = src->s_flags;
	dest->s_max_links = src->s_max_links;
	dest->s_mode = src->s_mode;
	dest->s_blocksize = src->s_blocksize;
	dest->s_blocksize_bits = src->s_blocksize_bits;
}


static int sfs_opts_parse(struct super_block *sb, char *str)
{
	int err = 0, first = 1, i;
	char *opt_str;
	struct super_block *replica_sb;
	struct sfs_sb_info *sbi, *replica_sbi;
	struct dentry *root, *replica_root;	
	struct sfs_dentry *sfs_root;
	unsigned int count;
	
	root = sb->s_root;
	sbi = sfs_master_sbi_alloc(sb);
	if(unlikely(IS_ERR(sbi))){
		err = PTR_ERR(sbi);
		printk(KERN_INFO "allocate sbi failed\n"); 
		goto out;
	}

	replica_count = 64;
	sfs_root = sfs_dentry_alloc(root);
	if(unlikely(IS_ERR(sfs_root))){
		err = PTR_ERR(sfs_root);
		printk(KERN_INFO "allocate sfs root dentry failed\n"); 
		goto out;
	}
	
	count = 0;
	while(!err && (opt_str = strsep(&str, ":")) &&*opt_str){

		struct path path1;
		char data[4096];

		strcpy(data, opt_str);

		err = kern_path(data, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path1);
		if(unlikely(err))
			return err;

		printk(KERN_INFO "replica fs path %s\n", opt_str);
		replica_sb = path1.mnt->mnt_sb;
		if(first){
			copy_super(sb, replica_sb);
			first = 0;
		 }

		replica_sbi = sfs_replica_sbi_alloc(replica_sb);
		if(unlikely(IS_ERR(replica_sbi))){
			err = PTR_ERR(replica_sbi);
			printk(KERN_INFO "allocate replica sbi failed\n"); 
			goto out;
		}
		replica_sbi->s_mnt = path1.mnt;

		s_add_replica(sbi, replica_sbi, count);
		dget(path1.dentry);
		replica_root = path1.dentry;
		replica_sbi->s_data_root = replica_root;

		printk(KERN_INFO "replica fs root path %s\n", replica_root->d_name.name);	
		d_add_replica(root, replica_root, count);

		count++;	 		
		path_put(&path1);
	}
	
	replica_count = count;
	sbi_array = kzalloc(count*sizeof(struct sfs_sb_info *), GFP_KERNEL);
	i = 0;
	list_for_each_replica(sbi, replica_sbi){
		sbi_array[i] = replica_sbi;	
		err = check_normal_dir(root, i);
		i++;
	}
	
out:
	return err;

}

static int sfs_fill_super (struct super_block *sb, void *data, int silent)
{

	int err;
	char *orig_data;
	
	printk(KERN_INFO "fill super\n");	
	orig_data = kstrdup(data, GFP_KERNEL);
	sb->s_op = &sfs_sops;
	sb->s_d_op = &sfs_dentry_operations;
	err = sfs_alloc_root(sb);
	if(err)
		goto out;

	printk(KERN_INFO "fill super 1\n");	
	err = sfs_opts_parse(sb, orig_data);

	printk(KERN_INFO "fill super 2 %d\n", err);	
	if(err)
		goto out;
	
//	err = assign_cpus(sb);
out:
	kfree(orig_data);
	return err;
}

static struct dentry *sfs_mount(struct file_system_type *fs_type,
			int flags, const char *dev_name, void *data)
{
	printk(KERN_INFO "mount \n");
	return mount_nodev(fs_type, flags, data, sfs_fill_super);

}

static void sfs_kill_sb(struct super_block *sb)
{	
	struct sfs_sb_info *sbi, *replica_sbi, *next;

	sbi = SFS_S(sb);
	printk(KERN_INFO "kill sb");
	if(!sbi)
		goto out;

	list_for_each_replica_safe(sbi, replica_sbi, next){
//		clear_dentry(replica_sbi->s_data_root);
//		dput(replica_sbi->s_data_root);
		list_del(&replica_sbi->u.r.replica_list);
		kfree(replica_sbi);
	}

	kfree(sbi);
	kfree(sbi_array);
	

out:
	generic_shutdown_super(sb);
	
}
static struct file_system_type sfs_fs_type = {
	.name		= "sfs",
	.mount		= sfs_mount,
	.kill_sb	= sfs_kill_sb,
};

static int __init init_sfs_fs(void)
{
	int err = 0;
	err = init_cpu2fs_mapping();
	if(err)
		goto out;

	err = init_filecache();
	if(err)
		goto out;

	err = init_dentrycache();
	if(err)
		goto out;

	err = register_filesystem(&sfs_fs_type);
	if(err)
		goto out;


out:
	return err;
}


static void __exit exit_sfs_fs(void)
{
	destroy_cpu2fs_mapping();
	destroy_filecache();
	destroy_dentrycache();
	unregister_filesystem(&sfs_fs_type);
}

MODULE_AUTHOR("Junbin Kang");
MODULE_LICENSE("GPL");

module_init(init_sfs_fs);
module_exit(exit_sfs_fs);

