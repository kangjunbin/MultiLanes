/** 
 * Copyright (C) 2013-2016 Junbin Kang. All rights reserved.
 */

#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/path.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/types.h>

#include "sfs_fs.h"

extern int replica_count;

static struct hlist_head **alloc_hash_table(int hash_len)
{
	int i;
	struct hlist_head **hash_table;

	hash_table = kmalloc(hash_len * sizeof(struct hlist_head *), GFP_KERNEL);
	if(!hash_table)
		return ERR_PTR(-ENOMEM);

	for(i = 0; i < hash_len; i++){
		hash_table[i] = kmalloc(sizeof(struct hlist_head), GFP_KERNEL);
		if(!hash_table[i])
			return ERR_PTR(-ENOMEM);
	
		INIT_HLIST_HEAD(hash_table[i]);	
	}
	return hash_table;
}

static void free_hash_table(struct hlist_head **hash_table, int hash_len)
{
	int i;

	for(i = 0; i < hash_len; i++)
		kfree(hash_table[i]);
	kfree(hash_table);		
}

static unsigned int caculate_hash_len(uint64_t size)
{
	unsigned int ret;
	unsigned int min_len = 1UL << 6;
	ret = size/AVARAGE_DIR_LEN;
	if(ret < min_len)
		ret = min_len;
	return ret;

}

static struct dir_info *sfs_alloc_dir_info(struct sfs_file *sfs_file)
{	
	struct dir_info *info;
	struct file *file = sfs_file->file;	

	info = kzalloc(sizeof(struct dir_info ), GFP_KERNEL);
	if(!info)
		return ERR_PTR(-ENOMEM);

	info->sfs_file = sfs_file;
	sfs_file->f_private_data = info;
	info->hash_len = caculate_hash_len(file->f_path.dentry->d_inode->i_size);
	info->dirent_bucket = alloc_hash_table(info->hash_len);

	return info;

}



static unsigned int dir_name_hash(const char *name, int namelen)
{
	return full_name_hash(name, namelen);
}

static inline int dir_string_cmp(const char *cs,const char *ct, unsigned int tcount)
{
	do{
		if(*cs != *ct)
			return 1;
		cs++;
		ct++;
		tcount--;
	}while (tcount);
	return 0;
}

static void sfs_free_hash_dirent(struct dir_info *dir_info)
{
	unsigned int hash;
	struct dir_entry *dir_entry;
	struct hlist_node* next;

	for(hash = 0; hash < dir_info->hash_len; hash++){
		hlist_for_each_entry_safe(dir_entry, next, dir_info->dirent_bucket[hash], hash_node){
			hlist_del(&dir_entry->hash_node);
			kfree(dir_entry);
		}
	}

	free_hash_table(dir_info->dirent_bucket, dir_info->hash_len);

}

static void sfs_refresh_hash_dirent(struct dir_info *dir_info)
{
	struct file *file = dir_info->sfs_file->file;
	unsigned int new_hash_len;

	new_hash_len = caculate_hash_len(file->f_path.dentry->d_inode->i_size);
	if(new_hash_len > dir_info->hash_len + (1 << 6)){
		sfs_free_hash_dirent(dir_info);
		dir_info->hash_len = new_hash_len;
		dir_info->dirent_bucket = alloc_hash_table(new_hash_len);
		
	}

}

/**
 * dir_lookup: This looks up the name in the hash table.
 * if it is found, return 1, else return 0
 */
static int dir_lookup(struct dir_info *dir_info, const char *name ,int namelen, unsigned int hash)
{
	int found = 0;
	struct dir_entry *dir_entry;
	unsigned int hash_index = hash % dir_info->hash_len;
	
	hlist_for_each_entry(dir_entry, dir_info->dirent_bucket[hash_index], hash_node){
		if(dir_entry->hash != hash)
			continue;
		if(dir_entry->name_len != namelen)
			continue;	

		if(!dir_string_cmp(dir_entry->name, name, namelen)){
			found = 1;
			break;
		}	
	}
	return found;
}

static void dir_hash_add(struct dir_info *dir_info, struct dir_entry *dir_entry)
{
	unsigned int hash_index = dir_entry->hash % dir_info->hash_len;

	hlist_add_head(&dir_entry->hash_node, dir_info->dirent_bucket[hash_index]);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
struct skip_callback {
	struct dir_context ctx;
	struct dir_info *dir_info;
	loff_t inline_pos;
};

struct readdir_callback {
	struct dir_context ctx;
	struct dir_context *orign_ctx;
	struct dir_info *dir_info;
	int count;
	int error;
};
#else
struct skip_callback {
	struct dir_info *dir_info;
	loff_t inline_pos;
};

struct readdir_callback {
	struct dir_info *dir_info;
	void *dirent;
	filldir_t filldir;
	int count;
	int error;
};
#endif


 
static int sfs_skip_filldir(void *__buf, const char *name, int namelen, loff_t offset,
	u64 d_ino, unsigned int d_type)
{
	struct skip_callback *skip_callback = (struct skip_callback *)__buf;
	struct dir_info *dir_info	= skip_callback->dir_info;
	loff_t inline_pos = skip_callback->inline_pos;
	
		
	if(dir_info->curr_minor_pos + namelen > inline_pos)
		return -EINVAL;
	else 
		dir_info->curr_minor_pos += namelen;

	return 0;	
}

static int sfs_dir_seek(struct file *file, loff_t f_pos)
{
	int err;
	struct sfs_file *sfs_file = SFS_F(file), *sfs_replica_file;
	struct file *replica_file;
	struct inode *replica_inode;
	struct dir_info *dir_info = DIR_INFO(sfs_file);
	loff_t inline_pos = f_pos;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
	struct skip_callback skip_callback = {
		.ctx.actor = sfs_skip_filldir,
		.dir_info = dir_info,
		.inline_pos = inline_pos
	};	
#else
	struct skip_callback skip_callback = {
		.dir_info = dir_info,
		.inline_pos = inline_pos
	};	
#endif
	loff_t size;

	dir_info->curr_file = NULL;
	dir_info->curr_major_pos = 0;
	dir_info->curr_minor_pos = 0;

	list_for_each_replica(sfs_file, sfs_replica_file){

		replica_file = sfs_replica_file->file;
		replica_inode = replica_file->f_path.dentry->d_inode;
		size = i_size_read(replica_inode);	
		if(inline_pos >= size){
			dir_info->curr_major_pos += size;
			inline_pos -= size;
		}
		else {
			dir_info->curr_file = sfs_replica_file;
			break;
		}				
	}
	
	if(unlikely(!dir_info->curr_file))
		return -EINVAL;

	lockdep_off();
	err = vfs_llseek(replica_file, 0, SEEK_SET);
	lockdep_on();

	if(inline_pos){
	
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
		lockdep_off();
		err = replica_file->f_op->iterate(replica_file, &skip_callback.ctx);
		lockdep_on();
#else
		lockdep_off();
		err = replica_file->f_op->readdir(replica_file, &skip_callback, sfs_skip_filldir);
		lockdep_on();
#endif
		if(err)
			goto errout;
		
	}
errout:	
	return err;
}

static int sfs_filldir(void *__buf, const char *name, int namelen, loff_t d_offset,
	u64 d_ino, unsigned int d_type)
{
	struct readdir_callback *readdir_callback = (struct readdir_callback *)__buf;
	struct dir_info *dir_info	= readdir_callback->dir_info;
	filldir_t filldir = readdir_callback->orign_ctx->actor;
	struct dir_entry *dir_entry;
	loff_t curr_pos;
	unsigned int hash = dir_name_hash(name, namelen);
	int err;
	/*
	 * if it is a dir, we check whether it is a duplicate entry using a hash table
         * which recorded read entries.
	 */
	if(d_type == DT_DIR || d_type == DT_UNKNOWN){

		if(dir_lookup(dir_info, name, namelen, hash)){
			dir_info->curr_minor_pos += namelen;
			return 0;	
		}

		/* 
		 * if not exsit, a new dirent is created and added to the hash table for eliminating duplicate entries.
		 */
		dir_entry = kmalloc(sizeof(struct dir_entry ), GFP_KERNEL);
		if(!dir_entry){
			return -ENOMEM;
		}

		dir_entry->offset	= d_offset;
		dir_entry->ino		= d_ino;
		dir_entry->name_len	= namelen;
		dir_entry->file_type	= d_type;
		memcpy(dir_entry->name, name, namelen);
		dir_entry->hash		= hash;
		dir_hash_add(dir_info, dir_entry);

	}

	curr_pos = dir_info->curr_major_pos + dir_info->curr_minor_pos;
	err = filldir(readdir_callback->orign_ctx, name, namelen, curr_pos, d_ino, d_type);

	if(!err)
		dir_info->curr_minor_pos += namelen;

	return err;
	
}

static loff_t end_of_file(struct file *file)
{
	struct file *replica_file;
	struct sfs_file *sfs_file = SFS_F(file), *sfs_replica_file;
	struct inode *inode;
	loff_t ret = 0;

	list_for_each_replica(sfs_file, sfs_file){
		replica_file = sfs_replica_file->file;
		inode = replica_file->f_path.dentry->d_inode;
		ret += i_size_read(inode);
	}
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
static int sfs_iterate(struct file *file, 
		struct dir_context *ctx)
{
	int error = 0;
	struct sfs_file *sfs_file = SFS_F(file), *sfs_replica_file;
	struct dir_info *dir_info = DIR_INFO(sfs_file);
	struct readdir_callback readdir_callback ={
		.ctx.actor = sfs_filldir,
		.orign_ctx = ctx
	};	
	
	if(!dir_info){
		dir_info = sfs_alloc_dir_info(sfs_file);
		if(IS_ERR(dir_info)){
			error = PTR_ERR(dir_info);
			goto errout;
		}
	} 
	
	/**
	 * As llseek happened since last readdir, We should seek to the indirect pos among fses.
	 */	
	if(ctx->pos != dir_info->last_pos){	
		sfs_refresh_hash_dirent(dir_info);
		error = sfs_dir_seek(file, ctx->pos);
		if(error)
			goto out;
	}

	if(!ctx->pos){
		sfs_replica_file = list_first_replica_file(sfs_file);
		dir_info->curr_file = sfs_replica_file;
		dir_info->curr_major_pos = 0;
		dir_info->curr_minor_pos = 0;
		lockdep_off();
		error = vfs_llseek(dir_info->curr_file->file, 0, SEEK_SET);
		lockdep_on();
	}

	/**
	 * readdir may reach the end of the file, but still need validate the condition.
	 */
	if(!dir_info->curr_file)
		if(!(dir_info->curr_file = next_replica_file(dir_info->end_file, sfs_file)))	
			return 0;
	

	/** 
	 * We combine the results read from multiple underlying fses
	 */
	sfs_replica_file = dir_info->curr_file;

	readdir_callback.dir_info = dir_info;
	list_for_each_replica_from(sfs_file, sfs_replica_file){
		dir_info->curr_file = sfs_replica_file;
		dir_info->end_file = sfs_replica_file;
		readdir_callback.count = 0;
		readdir_callback.error = 0;

		lockdep_off();
		readdir_callback.ctx.pos = dir_info->curr_file->file->f_pos;
		error = dir_info->curr_file->file->f_op->iterate(dir_info->curr_file->file, &readdir_callback.ctx);
		dir_info->curr_file->file->f_pos = readdir_callback.ctx.pos;
		lockdep_on();

		if(error)
			goto out;

		if(readdir_callback.error)
			goto out;
		/**
		 * Going here indicates that applications have successly read all entries of the replica file.
		 */
		dir_info->curr_major_pos += i_size_read(dir_info->curr_file->file->f_path.dentry->d_inode);		
		dir_info->curr_minor_pos = 0;
		lockdep_off();
		error = vfs_llseek(dir_info->curr_file->file, 0, SEEK_SET);
		lockdep_on();

	}	

	/**
	 * Going here indicates that applications have already read all entries of replica files.
	 * Null is assigned to curr_file pointer while keeping the dir_info->end_file point to the last read file.
	 */
	dir_info->curr_file = NULL;

out:	
	dir_info->last_pos = dir_info->curr_major_pos + dir_info->curr_minor_pos;
	ctx->pos = dir_info->last_pos;
errout:
	return error;
}
#else

static int sfs_readdir(struct file *file, 
		void *dirent, filldir_t filldir)
{
	int error = 0;
	struct sfs_file *sfs_file = SFS_F(file), *sfs_replica_file;
	struct dir_info *dir_info;
	struct readdir_callback readdir_callback;	
	
	dir_info = DIR_INFO(sfs_file);
	if(!dir_info){
		dir_info = sfs_alloc_dir_info(sfs_file);
		if(IS_ERR(dir_info)){
			error = PTR_ERR(dir_info);
			goto errout;
		}
	} 
	
	/**
	 * As llseek happened since last readdir, We should seek to the indirect pos among fses.
	 */	
	if(file->f_pos != dir_info->last_pos){	
		sfs_refresh_hash_dirent(dir_info);
		error = sfs_dir_seek(file, file->f_pos);
		if(error)
			goto out;
	}

	if(!file->f_pos){
		sfs_replica_file = list_first_replica_file(sfs_file);
		dir_info->curr_file = sfs_replica_file;
		dir_info->curr_major_pos = 0;
		dir_info->curr_minor_pos = 0;
		lockdep_off();
		error = vfs_llseek(dir_info->curr_file->file, 0, SEEK_SET);
		lockdep_on();
	}

	/**
	 * readdir may reach the end of the file, but still need validate the condition.
	 */
	if(!dir_info->curr_file)
		if(!(dir_info->curr_file = next_replica_file(dir_info->end_file, sfs_file)))	
			return 0;
	

	readdir_callback.dir_info = dir_info;
	readdir_callback.dirent = dirent;
	readdir_callback.filldir = filldir;	
	/** 
	 * We combine the results read from multiple underlying fses
	 */
	sfs_replica_file = dir_info->curr_file;

	list_for_each_replica_from(sfs_file, sfs_replica_file){
		dir_info->curr_file = sfs_replica_file;
		dir_info->end_file = sfs_replica_file;
		readdir_callback.count = 0;
		readdir_callback.error = 0;

		lockdep_off();
		error = dir_info->curr_file->file->f_op->readdir(dir_info->curr_file->file, &readdir_callback, sfs_filldir);
		lockdep_on();

		if(error)
			goto out;

		if(readdir_callback.error)
			goto out;
		/**
		 * Going here indicates that applications have successly read all entries of the replica file.
		 */
		dir_info->curr_major_pos += i_size_read(dir_info->curr_file->file->f_path.dentry->d_inode);		
		dir_info->curr_minor_pos = 0;
		lockdep_off();
		error = vfs_llseek(dir_info->curr_file->file, 0, SEEK_SET);
		lockdep_on();

	}	

	/**
	 * Going here indicates that applications have already read all entries of replica files.
	 * Null is assigned to curr_file pointer while keeping the dir_info->end_file point to the last read file.
	 */
	dir_info->curr_file = NULL;

out:	
	dir_info->last_pos = dir_info->curr_major_pos + dir_info->curr_minor_pos;
	file->f_pos = dir_info->last_pos;
errout:
	return error;
}
#endif

static int sfs_dir_open(struct inode *inode, struct file *file)
{
	int error = 0, loop;
	struct dentry *dentry, *replica_dentry;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;
	struct sfs_sb_info *replica_sbi;
	
	sfs_file = sfs_master_file_alloc(file);
	if(unlikely(IS_ERR(sfs_file))){
		error = PTR_ERR(sfs_file);
		goto out;
	}

	dentry = file->f_dentry;
	if(unlikely(!dentry)){
		error = -EIO;
		goto out;
	}

	for(loop = 0; loop < replica_count; loop++){
		
		struct path path;
		int flags;
		
		replica_dentry = find_replica_dentry(dentry, loop);
		if(!replica_dentry || !replica_dentry->d_inode)
			continue;

		if(!test_bitmap(SFS_D(dentry)->normal_dir_map, loop))
			continue;
	
		replica_sbi = get_replica_sbi(loop);
		path.mnt = replica_sbi->s_mnt;
		path.dentry = replica_dentry; 
		flags = file->f_flags;
		replica_file = dentry_open(&path, flags, current_cred());
		if(unlikely(IS_ERR(replica_file))){
			error = PTR_ERR(replica_file);
			goto out;
		}
		
		sfs_replica_file = sfs_replica_file_alloc(replica_file);
		if(unlikely(IS_ERR(sfs_replica_file))){
			error = PTR_ERR(sfs_replica_file);
			kfree(sfs_file);
			fput(replica_file);
			goto out;
			
		}

		f_add_replica(sfs_file, sfs_replica_file, loop);

	}

	file->f_version = inode->i_version;

out:
	return error;
	
}


static loff_t sfs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t maxsize;
	loff_t eof;
	
	maxsize = file->f_path.dentry->d_inode->i_sb->s_maxbytes;
	eof = end_of_file(file);
	return generic_file_llseek_size(file, offset, whence, maxsize, eof);
}

static int sfs_release_dir(struct inode *inode, struct file *file)
{
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file, *next;
	struct dir_info *dir_info;

	sfs_file = SFS_F(file);		

	list_for_each_replica_safe(sfs_file, sfs_replica_file, next){
		replica_file = sfs_replica_file->file;
		
		list_del_replica_file(sfs_replica_file);
		fput(replica_file);
	}

	dir_info = DIR_INFO(sfs_file);
	if(dir_info)
		sfs_free_hash_dirent(dir_info);	
	kfree(sfs_file);
	return 0;
}

int sfs_sync_dir(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret, err;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;
	
	sfs_file = SFS_F(file);
	list_for_each_replica(sfs_file, sfs_replica_file){
		replica_file = sfs_replica_file->file;
		
		mutex_lock(&inode->i_mutex);
		lockdep_off();
		err = vfs_fsync(replica_file, datasync);
		lockdep_on();
		mutex_unlock(&inode->i_mutex);
		if(!err){
			ret = err;
			goto out;
		}
	}
out:
	
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
const struct file_operations sfs_dir_operations = {
	.llseek		= sfs_dir_llseek,
	.read		= generic_read_dir,
	.iterate	= sfs_iterate,
	/*
	.unlocked_ioctl 	= sfs_ioctl,
#ifdef CONFIG_COMPAT	
	.compat_ioctl		= sfs_compat_ioctl,
#endif
	*/
	.fsync		= sfs_sync_dir,
	.open 		= sfs_dir_open,
	.release	= sfs_release_dir,
};

#else
const struct file_operations sfs_dir_operations = {
	.llseek		= sfs_dir_llseek,
	.read		= generic_read_dir,
	.readdir	= sfs_readdir,
	/*
	.unlocked_ioctl 	= sfs_ioctl,
#ifdef CONFIG_COMPAT	
	.compat_ioctl		= sfs_compat_ioctl,
#endif
	*/
	.fsync		= sfs_sync_dir,
	.open 		= sfs_dir_open,
	.release	= sfs_release_dir,
};
#endif
