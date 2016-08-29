/** 
 * Copyright (C) 2013-2016 Junbin Kang <kangjb@act.buaa.edu.cn>. All rights reserved.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/security.h>
#include <linux/mm.h>
#include <asm/uaccess.h>

#include "sfs_fs.h"
extern unsigned int get_replica_count(void);

extern void sfs_iput(struct dentry *dentry , struct inode *inode);
#define MAX_CPUS 80

struct list_head unused_sdentry_list[MAX_CPUS];
struct spinlock sdentry_list_lock[MAX_CPUS];

static struct kmem_cache *sfs_dentry_cachep;

int init_dentrycache(void)
{
	sfs_dentry_cachep = kmem_cache_create("sfs_dentry_cache",
				sizeof(struct sfs_dentry),
				0, (SLAB_RECLAIM_ACCOUNT|
				SLAB_MEM_SPREAD),
				NULL);
	if(sfs_dentry_cachep == NULL)
		return -ENOMEM;
	return 0;
}

void destroy_dentrycache(void)
{
	rcu_barrier();
	kmem_cache_destroy(sfs_dentry_cachep);

}

static void clean_sfs_dentry(struct sfs_dentry *sfs_dentry)
{
	int replica_count = get_replica_count();
	
	sfs_dentry->unused_replica_dentry = NULL;
	sfs_dentry->local_counter = 0;
	sfs_dentry->global_counter = 0;
	sfs_dentry->normal_dir_count = 0;
	sfs_dentry->replica_id = -1;
	sfs_dentry->meta_id = -1;
	sfs_dentry->normal_dir_map = 0;
	memset(sfs_dentry->replicas, 0, replica_count * sizeof(struct dentry *));
	spin_lock_init(&sfs_dentry->lock);


}

void clean_dentry(struct dentry *dentry)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	struct dentry *replica_dentry;
	int loop, replica_count = get_replica_count();	
	if(!sfs_dentry){
		printk(KERN_INFO "BUG: the sfs_dentry is missing\n");
		return;
	}
	for(loop = 0; loop < replica_count; loop++){
		replica_dentry = find_replica_dentry(dentry, loop);
		if(replica_dentry){
//			if(replica_dentry->d_lockref.count != 1)
//				printk(KERN_INFO "BUG: dentry %s count %d\n", replica_dentry->d_name.name, replica_dentry->d_lockref.count);	
			dput(replica_dentry);
		}
	}
	
	if(sfs_dentry->unused_replica_dentry)
		dput(sfs_dentry->unused_replica_dentry);

	clean_sfs_dentry(sfs_dentry);

}

static struct sfs_dentry *sfs_dentry_real_alloc(struct dentry *dentry)
{
	struct sfs_dentry *sfs_dentry;
	int replica_count = get_replica_count();

	sfs_dentry = kmem_cache_alloc(sfs_dentry_cachep, GFP_KERNEL);
	if(!sfs_dentry)
		return ERR_PTR(-ENOMEM);

	dentry->d_fsdata = sfs_dentry;
	sfs_dentry->unused_replica_dentry = NULL;

	sfs_dentry->local_counter = 0;
	sfs_dentry->global_counter = 0;
	sfs_dentry->normal_dir_count = 0;
	sfs_dentry->replica_id = -1;
	sfs_dentry->meta_id = -1;
	spin_lock_init(&sfs_dentry->lock);
	sfs_dentry->replicas = kzalloc(replica_count * sizeof(struct dentry *), GFP_KERNEL);
	
	if(unlikely(!sfs_dentry->replicas))
		return ERR_PTR(-ENOMEM);	
	
	sfs_dentry->normal_dir_map = 0;

	return sfs_dentry;
}

static inline struct sfs_dentry *get_sfs_dentry(struct dentry *dentry)
{
	struct sfs_dentry *sfs_dentry;
	
	sfs_dentry = sfs_dentry_real_alloc(dentry);

	return sfs_dentry;


}



struct sfs_dentry *sfs_dentry_alloc(struct dentry *dentry){

	return get_sfs_dentry(dentry);
}







/**
 * This is the callback from dput() in VFS when d_count is going to 0.
 */
static int sfs_dentry_delete(const struct dentry *dentry)
{
	int loop, replica_count = get_replica_count();
	struct sfs_dentry *sfs_dentry;
	struct dentry *replica_dentry;

	if(dentry->d_fsdata == NULL)
		return 0;
	
	sfs_dentry = SFS_D(dentry);
	if(!sfs_dentry)
		return 0;

	for(loop = 0; loop < replica_count; loop++){
		replica_dentry = find_replica_dentry(dentry, loop);

		d_drop(replica_dentry);
		dput(replica_dentry);
	}

	if(sfs_dentry->unused_replica_dentry)
		dput(sfs_dentry->unused_replica_dentry);

	kfree(sfs_dentry->replicas);
	kfree(sfs_dentry);
	return 0;
}

static void sfs_dentry_release(struct dentry *dentry)
{
	int loop, replica_count = get_replica_count();
	struct sfs_dentry *sfs_dentry;
	struct dentry *replica_dentry;

	if(dentry->d_fsdata == NULL)
		return;

	sfs_dentry = SFS_D(dentry);

	for(loop = 0; loop < replica_count; loop++){
		struct dentry *parent;
		replica_dentry = find_replica_dentry(dentry, loop);
		if(replica_dentry){
	//		printk(KERN_INFO "release bug p%d %s %d %d\n", loop, replica_dentry->d_name.name, replica_dentry->d_lockref.count, replica_dentry->d_parent->d_lockref.count);
			d_drop(replica_dentry);	
			parent = replica_dentry->d_parent;
			dput(replica_dentry);
	//		printk(KERN_INFO "release bug after %s %d\n", parent->d_name.name,  parent->d_lockref.count);
		}
	}

	if(sfs_dentry->unused_replica_dentry)
		dput(sfs_dentry->unused_replica_dentry);

	kfree(sfs_dentry->replicas);
	kfree(sfs_dentry);
	return; 
}

const struct dentry_operations sfs_dentry_operations = {
	.d_release	= sfs_dentry_release,
	.d_iput		= sfs_iput,
};

