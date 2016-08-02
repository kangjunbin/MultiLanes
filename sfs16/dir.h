#ifndef _SFS_DIR_H
#define _SFS_DIR_H
#include "file.h"

#define AVARAGE_DIR_LEN 32

struct dir_info {
	struct sfs_file *sfs_file;
	struct hlist_head **dirent_bucket;
	int hash_len;
	u32 total_nr_entries;
	loff_t last_pos;
	struct sfs_file *curr_file;
	struct sfs_file *end_file;
	loff_t curr_major_pos;
	loff_t curr_minor_pos;
	spinlock_t lock;
};


struct dir_entry {
	struct hlist_node hash_node;
	unsigned int hash;
	u64	offset;
	u32	ino;
	u8	name_len;
	u8	file_type;
	char name[1];
};

static inline struct dir_info *DIR_INFO(struct sfs_file *sfs_file)
{
	
	return sfs_file->f_private_data;

}

extern const struct file_operations sfs_dir_operations;
#endif
