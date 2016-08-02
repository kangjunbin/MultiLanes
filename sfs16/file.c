/** 
 * Copyright (C) 2013 Junbin Kang. All rights reserved.
 */
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "sfs_fs.h"

static struct kmem_cache *sfs_file_cachep;

int init_filecache(void)
{
	sfs_file_cachep = kmem_cache_create("sfs_file_cache",
				sizeof(struct sfs_file),
				0, (SLAB_RECLAIM_ACCOUNT|
				SLAB_MEM_SPREAD),
				NULL);
	if(sfs_file_cachep == NULL)
		return -ENOMEM;
	return 0;
}
void destroy_filecache(void)
{
	rcu_barrier();
	kmem_cache_destroy(sfs_file_cachep);

}

struct sfs_file *sfs_master_file_alloc(struct file *file)
{
	struct sfs_file *sfs_file;
	
	sfs_file = kmem_cache_alloc(sfs_file_cachep, GFP_KERNEL);
	if(!sfs_file)
		return ERR_PTR(-ENOMEM);

	file->private_data = sfs_file;
	sfs_file->file = file;
	sfs_file->f_private_data = NULL;
	INIT_LIST_HEAD(&sfs_file->u.m.replicas);
	sfs_file->u.m.meta_persistent = 0;
	sfs_file->u.m.is_partitioned = 0;

//	atomic_set(&sfs_file->u.m.replica_count, 0);
//	mutex_init(&sfs_file->u.m.mutex);
	
	return sfs_file;
}

struct sfs_file *sfs_replica_file_alloc(struct file *file)
{
	struct sfs_file *sfs_file;
	
	sfs_file = kmem_cache_alloc(sfs_file_cachep, GFP_KERNEL);
	if(!sfs_file)
		return ERR_PTR(-ENOMEM);

	sfs_file->file = file;
	INIT_LIST_HEAD(&sfs_file->u.r.replica_list);
	return sfs_file;
}
/*
 * sync file state between master file struct and replica file struct 
 */
void file_struct_sync(struct file *src, struct file *target)
{
	target->f_pos = src->f_pos;
	target->f_version = src->f_version;
	
}



void f_add_replica(struct sfs_file *sfs_master, struct sfs_file *sfs_replica, unsigned int replica_id)
{

	list_add_tail(&sfs_replica->u.r.replica_list, &sfs_master->u.m.replicas);
//	atomic_inc(&sfs_master->u.m.replica_count);
	sfs_replica->u.r.replica_id = replica_id;


} 

int sfs_new_file(struct file *master, struct file *replica, unsigned int replica_id)
{
	int error = 0;
	struct sfs_file *sfs_master, *sfs_replica;

	sfs_master = sfs_master_file_alloc(master);
	if(unlikely(IS_ERR(sfs_master))){
		error = PTR_ERR(sfs_master);
		goto out;
	}
		
	sfs_replica = sfs_replica_file_alloc(replica);
	if(unlikely(IS_ERR(sfs_replica))){
		error = PTR_ERR(sfs_replica);
		goto out;
	}

	f_add_replica(sfs_master, sfs_replica, replica_id);

out:	
	return error;	
}
/*
 * open a file or a dir 
 */
static int simple_file_open(struct path *path, struct file *filp, unsigned int replica_id)
{
	int error = 0, flags = filp->f_flags;
 	struct file *replica_filp;
	struct sfs_file *sfs_file, *sfs_replica_file;

	replica_filp = dentry_open(path, flags, current_cred());
	if(unlikely(IS_ERR(replica_filp))){
		error = PTR_ERR(replica_filp);
		goto out;
	}

		
	sfs_replica_file = sfs_replica_file_alloc(replica_filp);
	if(unlikely(IS_ERR(sfs_replica_file))){
		error = PTR_ERR(sfs_replica_file);
		fput(replica_filp);
		goto out;
	}

	sfs_file = SFS_F(filp);
	f_add_replica(sfs_file, sfs_replica_file, replica_id);

out: 
	return error;
	
}

static int sfs_file_open(struct inode *inode, struct file *flip)
{
	int error = 0, replica_id, meta_id;
	struct sfs_file *sfs_file;
	struct dentry *dentry, *replica_dentry, *meta_dentry;
	struct sfs_sb_info *replica_sbi, *meta_sbi;
	struct path path;

	dentry = flip->f_path.dentry;
	replica_id = get_replica_id(dentry);
	replica_dentry = SFS_D(dentry)->replicas[replica_id];
	replica_sbi = get_replica_sbi(replica_id);

	sfs_file = sfs_master_file_alloc(flip);
	if(unlikely(IS_ERR(sfs_file))){
		error = PTR_ERR(sfs_file);
		goto out;
	}

	path.mnt = replica_sbi->s_mnt;
	path.dentry = replica_dentry; 
	error = simple_file_open(&path, flip, replica_id);
	
	if(unlikely(error)){
		printk(KERN_INFO "open file error!");
		goto out;
	}
	
	meta_id = get_meta_id(dentry);
	if(meta_id == -1)
		goto out;

	sfs_file->u.m.is_partitioned = 1;
	meta_dentry = SFS_D(dentry)->replicas[meta_id];
	meta_sbi = get_replica_sbi(meta_id);
	
	path.mnt = meta_sbi->s_mnt;
	path.dentry = meta_dentry;
	error = simple_file_open(&path, flip, meta_id);

out: 
	return error;

}
static inline void file_pos_write(struct file *file, loff_t pos)
{
	file->f_pos = pos;
}

static inline loff_t file_pos_read(struct file *file)
{
	return file->f_pos;
}

loff_t sfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t ret;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;
	struct inode *inode = file->f_path.dentry->d_inode;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;

	mutex_lock(&inode->i_mutex);
	lockdep_off();
	ret = vfs_llseek(replica_file, offset, whence);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);

	spin_lock(&file->f_lock);
	file_struct_sync(replica_file, file);
	spin_unlock(&file->f_lock);


	return ret;
}

ssize_t sfs_file_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	ssize_t retval;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;

//	lockdep_off();
	retval = vfs_read(replica_file, buf, len, ppos);
//	lockdep_on();
	
	file_pos_write(replica_file, *ppos);

	return retval;
}

ssize_t sfs_file_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	ssize_t retval;
	struct file *replica_file;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;


	mutex_lock(&inode->i_mutex);
	lockdep_off();
	retval = vfs_write(replica_file, buf, len, ppos);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);
	
	file_pos_write(replica_file, *ppos);
	return retval;
}

ssize_t 
sfs_file_aio_write(struct kiocb *iocb, const struct iovec *iov, 
	unsigned long nr_segs, loff_t pos)
{
	struct file *file = iocb->ki_filp, *replica_file;
	struct inode *inode = file->f_path.dentry->d_inode;
	ssize_t retval;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;


	mutex_lock(&inode->i_mutex);
	lockdep_off();
	iocb->ki_filp = replica_file;
	retval = replica_file->f_op->aio_write(iocb, iov, nr_segs, pos);
	iocb->ki_filp = file;
	lockdep_on();
	mutex_unlock(&inode->i_mutex);
	file_pos_write(replica_file, pos);
	
	return retval;
		
}

ssize_t 
sfs_file_aio_read(struct kiocb *iocb, const struct iovec *iov, 
	unsigned long nr_segs, loff_t pos)
{
	struct file *file = iocb->ki_filp, *replica_file;
	ssize_t retval;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;

	iocb->ki_filp = replica_file;
	retval = replica_file->f_op->aio_read(iocb, iov, nr_segs, pos);
	iocb->ki_filp = file;

	file_pos_write(replica_file, pos);

	return retval;

}

long sfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	ssize_t retval;
	struct file *replica_file;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;


	mutex_lock(&inode->i_mutex);
	lockdep_off();
	retval = replica_file->f_op->unlocked_ioctl(replica_file, cmd, arg);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);

	return retval;
}
#ifdef CONFIG_COMPAT	
long sfs_compat_ioctl(struct file * file, unsigned int cmd, unsigned long arg)
{
	ssize_t retval;	
	struct file *replica_file;	
	struct inode *inode = file->f_path.dentry->d_inode;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;


	mutex_lock(&inode->i_mutex);
	lockdep_off();	
	retval = replica_file->f_op->compat_ioctl(replica_file, cmd, arg);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);

	return retval;
}
#endif

int sfs_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int error, replica_id;
	struct file *file = vma->vm_file, *replica_file;
	const struct vm_operations_struct *vm_ops;
	struct sfs_file *sfs_file, *sfs_replica_file;
	struct sfs_sb_info *replica_sbi;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;
	replica_id = sfs_replica_file->u.r.replica_id;
	
	replica_sbi = get_replica_sbi(replica_id);
	vm_ops = replica_sbi->vm_ops;

	vma->vm_file = replica_file;
	error = vm_ops->page_mkwrite(vma, vmf);
	vma->vm_file = file;

	return error;
}

int sfs_filemap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int error;
	struct file *file = vma->vm_file, *replica_file;
	const struct vm_operations_struct *vm_ops;
	struct sfs_file *sfs_file, *sfs_replica_file;
	struct sfs_sb_info *replica_sbi;
	unsigned int replica_id;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_id = sfs_replica_file->u.r.replica_id;
	replica_file = sfs_replica_file->file;
	replica_sbi = get_replica_sbi(replica_id);
	vm_ops = replica_sbi->vm_ops;
	
	vma->vm_file = replica_file;
	error = vm_ops->fault(vma, vmf);
	vma->vm_file = file;
	return error;

}

int sfs_file_remap_pages(struct vm_area_struct *vma, unsigned long addr, 
		unsigned long size, pgoff_t pgoff)
{
	int error;
	struct file *file = vma->vm_file, *replica_file;
	const struct vm_operations_struct *vm_ops;
	struct sfs_file *sfs_file, *sfs_replica_file;
	struct sfs_sb_info *replica_sbi;
	unsigned int replica_id;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_id = sfs_replica_file->u.r.replica_id;
	replica_file = sfs_replica_file->file;
	replica_sbi = get_replica_sbi(replica_id);
	vm_ops = replica_sbi->vm_ops;
	
	vma->vm_file = replica_file;
	error = vm_ops->remap_pages(vma, addr, size, pgoff);
	vma->vm_file = file;
	return error;

}

static const struct vm_operations_struct sfs_file_vm_ops = {
	.fault	= sfs_filemap_fault,
	.page_mkwrite	= sfs_page_mkwrite,
	.remap_pages	= sfs_file_remap_pages,
};

static int sfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	int error;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;
	struct sfs_sb_info *replica_sbi;
	unsigned int replica_id;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_id = sfs_replica_file->u.r.replica_id;
	replica_file = sfs_replica_file->file;
	replica_sbi = get_replica_sbi(replica_id);

	get_file(replica_file);
	vma->vm_file = replica_file;
	error = replica_file->f_op->mmap(replica_file, vma);	
	
	if(unlikely(error))
		goto out;
	
/*
	replica_sbi->vm_ops = vma->vm_ops;
	vma->vm_ops = &sfs_file_vm_ops;
*/

out:	
	fput(replica_file);
	return error;
}

static int sfs_release_file(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;
	
	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;
	
	list_del_replica_file(sfs_replica_file);
	fput(replica_file);

	if(sfs_file->u.m.is_partitioned){
		sfs_replica_file = list_first_replica_file(sfs_file);
		replica_file = sfs_replica_file->file;
		list_del_replica_file(sfs_replica_file);
		fput(replica_file);
	}
	
	kfree(sfs_file);
	return err;
}

int sfs_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;


//	mutex_lock(&inode->i_mutex);
//	lockdep_off();
	ret = vfs_fsync_range(replica_file, start, end, datasync);
//	lockdep_on();
//	mutex_unlock(&inode->i_mutex);

	return ret;
}

static ssize_t sfs_file_splice_read(struct file *in, loff_t *ppos,
		struct pipe_inode_info *pipe, size_t len,
		unsigned int flags)
{
	ssize_t retval;
	struct file *replica_file;
	struct sfs_file *sfs_file, *sfs_replica_file;
	
	sfs_file = SFS_F(in);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;
	retval = replica_file->f_op->splice_read(replica_file, ppos, pipe, len, flags);

	file_pos_write(replica_file, *ppos);	
	return retval;


}

static ssize_t sfs_file_splice_write(struct pipe_inode_info *pipe, struct file *out,
		loff_t *ppos, size_t len, unsigned int flags)
{
	ssize_t retval;
	struct file *replica_file;
	struct inode *inode = out->f_path.dentry->d_inode;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(out);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;

	mutex_lock(&inode->i_mutex);
	lockdep_off();
	retval = replica_file->f_op->splice_write(pipe, replica_file, ppos, len, flags);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);

	file_pos_write(replica_file, *ppos);	

	return retval;


}
	
static long sfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	int ret;
	struct file *replica_file;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct sfs_file *sfs_file, *sfs_replica_file;

	sfs_file = SFS_F(file);
	sfs_replica_file = list_first_replica_file(sfs_file);
	replica_file = sfs_replica_file->file;


	mutex_lock(&inode->i_mutex);
	lockdep_off();
	ret = replica_file->f_op->fallocate(replica_file, mode, offset, len);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);

	return ret;

}

/**
 * for madvise
 */
static int sfs_readpage(struct file *file, struct page *page)
{	
	unlock_page(page);
	return 0;
}

/**
 * it will never be called, but necessary to support O_DIRECT
 */
static ssize_t sfs_direct_IO(int rw, struct kiocb *iocb,
			const struct iovec *iov, loff_t offset,
			unsigned long nr_segs)
{return 0;}

static int sfs_get_xip_mem(struct address_space *mapping, pgoff_t pgoff,
		int create, void **kmem, unsigned long *pfn)
{return 0;}

const struct file_operations sfs_file_operations = {
	.llseek		= sfs_file_llseek,
	.read		= sfs_file_read,
	.write		= sfs_file_write,
	.aio_read	= sfs_file_aio_read,
	.aio_write	= sfs_file_aio_write,
	.unlocked_ioctl	= sfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sfs_compat_ioctl,
#endif
	.mmap		= sfs_file_mmap,
	.open		= sfs_file_open,
	.release	= sfs_release_file,
	.fsync		= sfs_sync_file,
	.splice_read	= sfs_file_splice_read,
	.splice_write	= sfs_file_splice_write,
	.fallocate	= sfs_fallocate,
};

const struct inode_operations sfs_file_inode_operations = {
	.setattr	= sfs_setattr,
	.getattr	= sfs_getattr,
	.fiemap		= sfs_fiemap,
};


const struct address_space_operations sfs_aop = {
	.readpage 	= sfs_readpage,
	.direct_IO	= sfs_direct_IO,
	.get_xip_mem	= sfs_get_xip_mem,
};

