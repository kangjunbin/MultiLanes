#ifndef _SFS_FILE_H
#define _SFS_FILE_H

#include <linux/file.h>

#include "sfs_fs.h"

struct sfs_file {
	struct file *file;
	
	void *f_private_data;
	union {
		struct {
			struct list_head replicas;
			//atomic_t replica_count;
			//struct mutex mutex;
			int is_partitioned;
			int meta_persistent;

		}m;
		struct {
			struct list_head replica_list;
			struct sfs_file *master;
			unsigned int replica_id;
		}r;
	}u;
};

static inline struct sfs_file * SFS_F(const struct file *file)
{
	return file->private_data;

}



extern void file_struct_sync(struct file *file1, struct file *file2);

extern void f_add_replica(struct sfs_file *master, struct sfs_file *replica, unsigned int replica_id);

extern struct sfs_file *sfs_master_file_alloc(struct file *file);

extern struct sfs_file *sfs_replica_file_alloc(struct file *file);

extern int sfs_new_file(struct file *master, struct file *replica, unsigned int replica_id);

extern int init_filecache(void);

extern void destroy_filecache(void);


extern const struct file_operations sfs_file_operations;

extern const struct inode_operations sfs_file_inode_operations;

extern const struct address_space_operations sfs_aop;

#endif
