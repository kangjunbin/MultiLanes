#ifndef _SFS_FS_H
#define _SFS_FS_H
#include <asm/atomic.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/rculist.h>
#include <linux/list.h>
#include <linux/aio.h>
#include <linux/version.h>
#include <linux/mount.h>
#include <linux/sched.h>

#include "namei.h"
#include "file.h"
#include "dir.h"
#include "inode.h" 
#include "dentry.h"
#include "dispatch.h"
#include "kv.h"

//#define DEBUG 1
#define CONCURRENCY_THRESHOLD	4
#define SPAN_THRESHOLD 20	
//#define PARTITIONED 1 

/**
 * Flags for indicating which type of directory to create: normal or shadow
 */

#define CLONE_NORMAL 1
#define CLONE_SHADOW 0

struct sfs_sb_info {
	struct super_block *s_sb;
	struct vfsmount *s_mnt;
	struct dentry *s_data_root;
	struct dentry *s_special;

	int s_replica_id;
	const struct vm_operations_struct *vm_ops;
	union {
		struct {

			struct list_head replicas;
			atomic_t replica_count;
		}m;
		struct {
			struct list_head replica_list;
			struct sfs_sb_info *master;
		}r;
	}u;

	spinlock_t lock;
	struct cpumask cpu_set;

	/* account the number of cpus which simultaneously access to the fs */
	unsigned long nr_accessing_cpus;  

	/**
          * account the number of processes per cpu 
          * which are accessing to the fs at this time 
 	  */
	unsigned long nr_accessing_processes_percpu[NR_CPUS];  
	
};


static inline struct sfs_sb_info *SFS_S(const struct super_block *sb)
{
	return sb->s_fs_info;
}

struct sfs_dentry {
	struct dentry **replicas;
	struct dentry *unused_replica_dentry;
	/* 
	 *for regular file, it points where the only file object exists; 
	 *for dir, it points where the metadata parent directory lies;
	 */
	int replica_id;   //point to the data file
	int meta_id;   //point to the meta file

	/*
	 * They are used for dispatching
	 */
	int local_counter;
	int global_counter;

	/*
	 * They are used for shadow directory
	 */
	int normal_dir_count;

	int first_normal_dir_id;
	
	/* Normal directory */
	uint64_t normal_dir_map;
	
	

	spinlock_t lock;
};


static inline 
struct sfs_dentry *SFS_D(const struct dentry *dentry)
{
	return dentry->d_fsdata;
}

static inline 
void set_replica_id(const struct dentry *dentry, int replica_id)
{
	SFS_D(dentry)->replica_id = replica_id;
}

static inline
void set_meta_id(const struct dentry *dentry, int meta_id)
{
	SFS_D(dentry)->meta_id = meta_id;
}

static inline 
int get_replica_id(const struct dentry *dentry)
{
	
	return SFS_D(dentry)->replica_id;
}

static inline
int get_meta_id(const struct dentry *dentry)
{
	return SFS_D(dentry)->meta_id;
}

static inline 
struct dentry *find_replica_dentry(const struct dentry *dentry, int replica_id)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);

	return sfs_dentry->replicas[replica_id];
}
static inline
struct dentry *list_first_replica_dentry(const struct dentry *dentry)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	
	return sfs_dentry->replicas[sfs_dentry->replica_id];
}

static inline
void clear_dentry(struct dentry *dentry)
{

	return;
#ifdef DEBUG
	printk(KERN_INFO "replica_dentry %s %d\n", dentry->d_name.name, dentry->d_lockref.count);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
	if(dentry->d_lockref.count == 1)
		return;
	
	spin_lock(&dentry->d_lockref.lock);
	dentry->d_lockref.count = 1;
	spin_unlock(&dentry->d_lockref.lock);

#else
	if(dentry->d_count == 1)
		return;

	spin_lock(&dentry->d_lock);
	dentry->d_count	= 1;
	spin_unlock(&dentry->d_lock);
#endif

}
static inline
void clear_root_dentry(struct dentry *dentry)
{
	return;
#ifdef DEBUG
	printk(KERN_INFO "replica_dentry %s %d\n", dentry->d_name.name, dentry->d_lockref.count);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
	if(dentry->d_lockref.count == 2)
		return;
	
	spin_lock(&dentry->d_lockref.lock);
	dentry->d_lockref.count = 2;
	spin_unlock(&dentry->d_lockref.lock);

#else
	if(dentry->d_count == 1)
		return;

	spin_lock(&dentry->d_lock);
	dentry->d_count	= 2;
	spin_unlock(&dentry->d_lock);
#endif

}
/* 
 * get replica file of the master file, note that for regular files, 
 * there is only one replica in the replica list of master
 */
static inline  	
struct sfs_file *list_first_replica_file(struct sfs_file *sfs_file)
{
	struct sfs_file *sfs_replica_file;

	sfs_replica_file = list_first_entry(&sfs_file->u.m.replicas, struct sfs_file, u.r.replica_list);

	return sfs_replica_file;
}

static inline
struct sfs_file *list_last_replica_file(struct sfs_file *sfs_file)
{
	struct sfs_file *sfs_replica_file;
	
	sfs_replica_file = list_entry(sfs_file->u.m.replicas.prev, struct sfs_file, u.r.replica_list);

	return sfs_replica_file;
}

static inline
struct sfs_file *next_replica_file(struct sfs_file *sfs_replica_file, struct sfs_file *sfs_file)
{
	
	if(list_is_last(&sfs_replica_file->u.r.replica_list, &sfs_file->u.m.replicas))
		return NULL;
	
	return list_entry(sfs_replica_file->u.r.replica_list.next, struct sfs_file, u.r.replica_list);

}

static inline
void list_del_replica_file(struct sfs_file *sfs_replica)
{
	list_del(&sfs_replica->u.r.replica_list);
	kfree(sfs_replica);
}

enum {
	I_MUTEX_OTHER_BEGIN = I_MUTEX_QUOTA,
	I_MUTEX_OTHER_NORMAL,
	I_MUTEX_OTHER_PARENT,
	I_MUTEX_OTHER_CHILD,
	I_MUTEX_OTHER_END
};

enum {
	S_RENAME_SFS_BEGIN,
	S_RENAME_SFS
};

inline static
uint64_t test_bitmap(uint64_t bitmap, int shift)
{
	uint64_t ret;	
	ret =  ((unsigned long )1 << shift) & bitmap; 
	return ret;
}

inline static
void set_bitmap(uint64_t *bitmap, int shift)
{
	*bitmap = *bitmap | ((unsigned long )1 << shift);

}

#define NR_REPLICAS(sfs_ptr) atomic_read(&(sfs_ptr)->u.m.replica_count)

#define list_for_each_replica(master, replica) \
	list_for_each_entry(replica, &(master)->u.m.replicas, u.r.replica_list)

#define list_for_each_replica_safe(master, replica, next) \
	list_for_each_entry_safe(replica, next, &(master)->u.m.replicas, u.r.replica_list)

#define list_for_each_replica_continue(master, replica) \
	list_for_each_entry_continue(replica, &(master)->u.m.replicas, u.r.replica_list)

#define list_for_each_replica_from(master, replica) \
	list_for_each_entry_from(replica, &(master)->u.m.replicas, u.r.replica_list)

extern struct sfs_sb_info *get_replica_sbi(int replica_id);
#endif
	
