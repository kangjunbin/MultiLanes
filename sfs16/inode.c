/** 
 * Copyright (C) 2013 Junbin Kang. All rights reserved.
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




void sfs_iput(struct dentry *dentry , struct inode *inode)
{
	iput(inode);
}

static void sfs_set_aops(struct inode *inode)
{
	inode->i_mapping->a_ops = &sfs_aop;
}

struct inode *sfs_alloc_inode(struct super_block *sb)
{
	struct inode *inode;
	
	inode = new_inode(sb);
	inode->i_ino = get_next_ino();

	if(unlikely(!inode))
		return ERR_PTR(-ENOMEM);

	return inode;
}

int sfs_alloc_root(struct super_block *sb)
{
	int err = 0;
	struct inode *inode;
	struct dentry *root;

	inode = sfs_alloc_inode(sb);
	if(unlikely(IS_ERR(inode))){
		err = PTR_ERR(inode);
		goto out;
	}

	inode->i_mode = S_IFDIR;
	set_nlink(inode, 2);
	inode->i_op = &sfs_dir_inode_operations;
	inode->i_fop = &sfs_dir_operations;
	 
	inode->i_mode = S_IFDIR;
//	unlock_new_inode(inode);

	root = d_make_root(inode);
	if(!unlikely(root))
		goto out;

	if(unlikely(IS_ERR(root))){
		err = PTR_ERR(root);
		goto out;
	}

	sb->s_root = root;
out:
	return err;

}

struct inode *sfs_new_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode;
	long ret;
	
	inode = sfs_alloc_inode(sb);
	if(unlikely(IS_ERR(inode)))
		return inode;

	inode->i_mode = mode;

	switch(inode->i_mode & S_IFMT){
		case S_IFREG:
			inode->i_op = &sfs_file_inode_operations;
			inode->i_fop = &sfs_file_operations;
			sfs_set_aops(inode);
			break;
		case S_IFDIR:
			inode->i_op = &sfs_dir_inode_operations;
			inode->i_fop = &sfs_dir_operations;
			break;
		case S_IFLNK:
			inode->i_op = &sfs_symlink_inode_operations;
			sfs_set_aops(inode);
			break;
		default:
			ret = -EIO;
			goto bad_inode;
			break;
	}

//	unlock_new_inode(inode);
	return inode;
bad_inode:
	printk(KERN_INFO "bad inode \n");
	return ERR_PTR(ret);
}
/*
int sfs_setattr_1(struct dentry *dentry, struct iattr *ia)
{
	int err, is_dir = S_ISDIR(dentry->d_inode->i_mode);
	struct sfs_dentry *sfs_dentry, *sfs_replica_dentry;
	struct inode *inode, *replica_inode;
	struct dentry *replica_dentry;
	struct file *file;
	cpumask_t saved_mask;
	unsigned int replica_id, dest_cpu = -1;

	inode = dentry->d_inode;
	
	if(ia->ia_valid &(ATTR_KILL_SUID | ATTR_KILL_SGID))
		ia->ia_valid &= ~ATTR_MODE;

	file = NULL;
	
	sfs_dentry = SFS_D(dentry);
	d_lock(sfs_dentry);
	list_for_each_replica(sfs_dentry, sfs_replica_dentry){
		struct path path;
		struct sfs_sb_info *replica_sbi;

		d_unlock(sfs_dentry);

		replica_dentry = sfs_replica_dentry->dentry;
		replica_inode = replica_dentry->d_inode;
		replica_id = sfs_replica_dentry->replica_id;
		replica_sbi = get_replica_sbi(replica_id);

		path.mnt = replica_sbi->s_mnt;
		path.dentry = replica_dentry;
	
		mutex_lock_nested(&replica_inode->i_mutex, I_MUTEX_OTHER_NORMAL);
		if(!is_dir)
			migrate_on_update(replica_id, &saved_mask, &dest_cpu);

		file = NULL;	
		if(ia->ia_valid & ATTR_SIZE){

			struct file *f;

			if(!S_ISREG(replica_inode->i_mode)){
				err = -EINVAL;
				mutex_unlock(&replica_inode->i_mutex);
				goto out;
			}
			
			f = NULL;
			if(ia->ia_valid & ATTR_FILE){
				file = ia->ia_file;
				ia->ia_file = list_first_replica_file(SFS_F(file))->file;
				f = ia->ia_file;
			}
			if(ia->ia_size < i_size_read(replica_inode))
				truncate_setsize(replica_inode, ia->ia_size);
			
			mutex_unlock(&replica_inode->i_mutex);
			lockdep_off();
			err = vfs_truncate(&path, ia->ia_size);
			lockdep_on();
			mutex_lock_nested(&replica_inode->i_mutex, I_MUTEX_OTHER_NORMAL);
		}else
			err = notify_change(replica_dentry, ia);

		if(file){
			ia->ia_file = file;
			ia->ia_valid |= ATTR_FILE;
		}

		mutex_unlock(&replica_inode->i_mutex);
		d_lock(sfs_dentry);
	}
	d_unlock(sfs_dentry);

	setattr_copy(dentry->d_inode, ia);

out:
	if(!is_dir)
		finish_update(replica_id, &saved_mask, dest_cpu);
	return err;

}
*/
/**
 * can only truncate a regular file 
 */
int sfs_file_setattr(struct dentry *dentry, struct iattr *attr)
{
	int err;
	struct dentry *replica_dentry;
	cpumask_t saved_mask;
	unsigned int replica_id, dest_cpu = -1;

	replica_id = get_replica_id(dentry);
	migrate_on_update(replica_id, &saved_mask, &dest_cpu);
	
	replica_dentry = find_replica_dentry(dentry, replica_id);
	/*
	 * As setattr is called by notify_change, using notify_change is better ?  
 	 */
	mutex_lock_nested(&replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_NORMAL);
	err = notify_change(replica_dentry, attr);
		//err = replica_dentry->d_inode->i_op->setattr(replica_dentry, attr);

	mutex_unlock(&replica_dentry->d_inode->i_mutex);
	if(err)
		goto out;


	/* update the attr of inode in sfs */
	setattr_copy(dentry->d_inode, attr);

out:
	finish_update(replica_id, &saved_mask, dest_cpu);
	return err;

}
int sfs_dir_setattr(struct dentry *dentry, struct iattr *attr)
{
	int err, loop, replica_count = get_replica_count();
	struct dentry *replica_dentry;

	if(attr->ia_valid & ATTR_SIZE)
		if(S_ISDIR(dentry->d_inode->i_mode))
			return -EIO;

	
	for(loop = 0; loop < replica_count; loop++){
		

		replica_dentry = find_replica_dentry(dentry, loop);
		if(unlikely(!replica_dentry))
			continue;
		
		if(!replica_dentry->d_inode)
			continue;

		/*
		 * As setattr is called by notify_change, using notify_change is better ?  
 		 */
		mutex_lock_nested(&replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_NORMAL);
		err = notify_change(replica_dentry, attr);
		//err = replica_dentry->d_inode->i_op->setattr(replica_dentry, attr);

		mutex_unlock(&replica_dentry->d_inode->i_mutex);
		if(err)
			goto out;

	}

	/* update the attr of inode in sfs */
	setattr_copy(dentry->d_inode, attr);

out:
	return err;

}
int sfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	if(S_ISDIR(dentry->d_inode->i_mode))
		return sfs_dir_setattr(dentry, attr);
	 else 
		return sfs_file_setattr(dentry, attr);	

}

static int sfs_dir_getattr(struct vfsmount *mnt, struct dentry *dentry,
	struct kstat *stat)
{
	struct dentry *replica_dentry;
	struct inode *replica_inode;
	struct vfsmount *replica_mnt;
	struct kstat tmp;
	struct kstat *p;
	int is_first = 1, loop, replica_count = get_replica_count();

	for(loop = 0; loop < replica_count; loop++){
		replica_dentry = find_replica_dentry(dentry, loop);
		replica_inode = replica_dentry->d_inode;
		replica_mnt = SFS_S(replica_dentry->d_sb)->s_mnt;
			
		if(is_first)
			p = stat;
		else 
			p = &tmp;
		if(replica_inode->i_op->getattr)
			replica_inode->i_op->getattr(replica_mnt, replica_dentry, p);
		else 
			generic_fillattr(replica_inode, p);
		if(is_first)
			is_first = 0;
		else {
			stat->nlink += p->nlink;
			stat->size  += p->size;
			stat->blocks += p->blocks;
		}
	}

	stat->ino = dentry->d_inode->i_ino;
	return 0;	

}
static int sfs_file_getattr(struct vfsmount *mnt, struct dentry *dentry,
	struct kstat *stat)
{	
	struct dentry *replica_dentry;
	struct inode *replica_inode;
	struct vfsmount *replica_mnt;
	unsigned int replica_id;
	struct sfs_sb_info *replica_sbi;

	replica_dentry = list_first_replica_dentry(dentry);
	replica_id = get_replica_id(dentry);
	replica_sbi = get_replica_sbi(replica_id);

	replica_inode = replica_dentry->d_inode;
	replica_mnt = replica_sbi->s_mnt;

	if(replica_inode->i_op->getattr)
		return replica_inode->i_op->getattr(replica_mnt, replica_dentry, stat);
	
	generic_fillattr(replica_inode, stat);
	return 0;
}
int sfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
	struct kstat *stat)
{
	if(S_ISDIR(dentry->d_inode->i_mode))
		return sfs_dir_getattr(mnt, dentry, stat);
	 else 
		return sfs_file_getattr(mnt, dentry, stat);	
}

int sfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
	__u64 start, __u64 end)
{
	int err;
	struct dentry *dentry, *replica_dentry;
	struct inode *replica_inode;

	dentry = d_find_any_alias(inode);
	replica_dentry = list_first_replica_dentry(dentry);
	replica_inode = replica_dentry->d_inode;

	if(!replica_inode->i_op->fiemap){
		err = -EACCES;
		goto out;
	}
	
	mutex_lock(&inode->i_mutex);
	lockdep_off();
	replica_inode->i_op->fiemap(replica_inode, fieinfo, start, end);
	lockdep_on();
	mutex_unlock(&inode->i_mutex);

out:
	dput(dentry);
	return err;
}


