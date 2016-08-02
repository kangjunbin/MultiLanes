/** 
 * Copyright (C) 2013 Junbin Kang. All rights reserved.
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

#include "sfs_fs.h"

extern int sfs_rmdir(struct inode *dir, struct dentry *dentry);
extern unsigned int get_replica_count(void);
struct dentry *d_ancestor(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for(p = p2; !IS_ROOT(p); p = p->d_parent) {
		if(p->d_parent == p1)
			return p;
	}
	return NULL;
}

struct dentry *sfs_lock_rename(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	if (p1 == p2) {
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_OTHER_PARENT);
		return NULL;
	}

	mutex_lock_nested(&p1->d_inode->i_sb->s_vfs_rename_mutex, S_RENAME_SFS);

	p = d_ancestor(p2, p1);
	if (p) {
		mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_OTHER_PARENT);
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
		return p;
	}

	p = d_ancestor(p1, p2);
	if (p) {
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_OTHER_PARENT);
		mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
		return p;
	}

	mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_OTHER_PARENT);
	mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
	return NULL;
}

void sfs_unlock_rename(struct dentry *p1, struct dentry *p2)
{
	mutex_unlock(&p1->d_inode->i_mutex);
	if (p1 != p2) {
		mutex_unlock(&p2->d_inode->i_mutex);
		mutex_unlock(&p1->d_inode->i_sb->s_vfs_rename_mutex);
	}
}

void reset_replica_dentry(struct dentry *target, int replica_id)
{
	struct dentry *target_replica_dentry;	
	
	target_replica_dentry = find_replica_dentry(target, replica_id);
//	printk(KERN_INFO "reset %s %d\n", target->d_name.name, replica_id);
	
	if(target_replica_dentry){

		SFS_D(target)->replicas[replica_id] == NULL;	
//		clear_dentry(target_replica_dentry);
		dput(target_replica_dentry);
	}
}

void sfs_d_move(struct dentry *dentry, struct dentry *target, int replica_id)
{
	int loop, replica_count = get_replica_count();
	struct dentry *replica_dentry, *target_replica_dentry, *tmp;


	for(loop = 0; loop < replica_count; loop++){
		if(loop == replica_id)
			continue;

		replica_dentry = find_replica_dentry(dentry, loop);
		target_replica_dentry = find_replica_dentry(target, loop);
		
		tmp = SFS_D(dentry)->replicas[loop];
		SFS_D(dentry)->replicas[loop] = target_replica_dentry;
		SFS_D(target)->replicas[loop] = tmp;
				
	}

}
			
static int sfs_rename_dir(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	int error = 0, loop, replica_id;
	struct dentry *old_parent, *new_parent,
		*old_replica_parent, *new_replica_parent, 
		*old_replica_dentry, *new_replica_dentry;
	int replica_count = get_replica_count();
	umode_t mode;

	/**
	 * A dir can't be renamed to a existed non-emptry dir. 
	 * We first remove the new dentry, if failed, the result may indicate that the new dentry is a non-empty dir. 
	 * This failed operation does not harm the consistent view of sfs as only empty replica dires are removed 
	 * and non-empty dires are left.
	 */
	if(new_dentry->d_inode){
		error = sfs_rmdir(new_dir, new_dentry);
#ifdef DEBUG
		printk(KERN_INFO "rename rmdir %s %d\n",new_dentry->d_name.name, error);
#endif
		if(error)
			goto out;
	}

	old_parent = old_dentry->d_parent;
	new_parent = new_dentry->d_parent;
	mode = old_dentry->d_inode->i_mode;

	
	for(loop = 0; loop < replica_count; loop++){
		old_replica_dentry = find_replica_dentry(old_dentry, loop);
		if(!old_replica_dentry || !old_replica_dentry->d_inode){
			SFS_D(old_dentry)->replicas[loop] = 0;			
			continue;
		}

		old_replica_parent = find_replica_dentry(old_parent, loop);

		if(unlikely(!old_replica_parent->d_inode)){
			error = -EIO;
			goto revert;
		}
		
				
		new_replica_parent = create_replica_dir(new_parent, loop, CLONE_SHADOW);
		
		new_replica_dentry = get_replica_dentry(new_replica_parent, new_dentry, loop);
		if(unlikely(IS_ERR(new_replica_dentry))){
			error = PTR_ERR(new_replica_dentry);
			goto revert;
		}				
//		printk(KERN_INFO "rename %s %s -> %s %s\n", old_replica_parent->d_name.name, old_replica_dentry->d_name.name, 
							//	new_replica_parent->d_name.name, new_replica_dentry->d_name.name);

		sfs_lock_rename(new_replica_parent, old_replica_parent);

		error = old_replica_parent->d_inode->i_op->rename(old_replica_parent->d_inode, old_replica_dentry, 
							new_replica_parent->d_inode, new_replica_dentry);
//		printk(KERN_INFO "rename %d %d\n", error, loop);
		sfs_unlock_rename(new_replica_parent, old_replica_parent);
		/**
		 * Error may harm the consistent view of sfs as operations are partially handled among all replicas.
		 * How to solve this problem?  Rollbacking and Operation-based write-ahead mechanism can be used to address the issue.
		 */
		if(likely(!error)){
	
			if(!(old_replica_parent->d_inode->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE))
				d_move(old_replica_dentry, new_replica_dentry);
		}

	}
	
		
	
	/* We should add a directory entry under a normal parent directory */
	replica_id = SFS_D(new_parent)->first_normal_dir_id;
	if(replica_id == -1)
		replica_id = dispatch();
					
	error = simple_add_dir_entry(new_dentry, mode, replica_id, CLONE_NORMAL);
	
	if(unlikely(error))
		goto revert;



	goto out;
revert:
	/*unimplemented, future work? */
	
out:
	return error;		

}

/* The corresponding operations for Garbage Collection (GC): 
 * 1. If the data file of one partitioned file has a rename link and the rename link points to an empty location or itself, 
 *    GC first removes the rename link and then checks the point link of it as normal.
 * 2. If the data file of one partitioned file has a stale link and the stale link points to an empty location,
 *    GC does the same as the above case.
 */

static int rename2non(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	int err = 0, old_replica_id, old_meta_id, new_meta_id, can_turn, clone_type = CLONE_SHADOW;
	struct dentry *old_parent, *new_parent,
		*old_replica_dentry, *new_replica_dentry,
		*old_replica_parent, *new_replica_parent,
		*new_meta_parent, *new_meta_dentry,
		*old_meta_parent, *old_meta_dentry;
	umode_t mode;
	int pointer[4] = {-1,-1,-1, -1};
	
	old_replica_dentry = list_first_replica_dentry(old_dentry);
	old_replica_id = get_replica_id(old_dentry);
	old_meta_id = get_meta_id(old_dentry);
	
	old_parent = old_dentry->d_parent;
	new_parent = new_dentry->d_parent;
	
	old_replica_parent = find_replica_dentry(old_parent, old_replica_id);


	can_turn = can_turn2normal(new_parent, old_replica_id);
	if(can_turn)
		clone_type = CLONE_NORMAL;
	else
		clone_type = CLONE_SHADOW;

	new_replica_parent = create_replica_dir(new_parent, old_replica_id, clone_type);	
	if(unlikely(IS_ERR(new_replica_parent))){
		err = PTR_ERR(new_replica_parent);
		goto out;
	}

	new_replica_dentry = get_replica_dentry(new_replica_parent, new_dentry, old_replica_id);
	if(unlikely(IS_ERR(new_replica_dentry))){
		err = PTR_ERR(new_replica_dentry);
		goto out;
	}

	
	if(can_turn){
#ifdef DEBUG
		printk(KERN_INFO "rename2non: can_turn %s %s\n", old_replica_dentry->d_name.name, new_replica_dentry->d_name.name);
#endif
		if(old_meta_id != -1){
			/* Add a rename link to make the link point to itself. */ 
			pointer[RENAME] = old_replica_id;
			err = put(old_replica_dentry, "trusted.sfs.meta", pointer);
			if(unlikely(err))
				goto out;
			
			write_inode_now(old_replica_dentry->d_inode, 1);
		}

		sfs_lock_rename(new_replica_parent, old_replica_parent);
		err = old_replica_parent->d_inode->i_op->rename(old_replica_parent->d_inode, old_replica_dentry, 
						new_replica_parent->d_inode, new_replica_dentry);
		sfs_unlock_rename(new_replica_parent, old_replica_parent);
		if(unlikely(err))
			goto out;
		
		d_move(old_replica_dentry, new_replica_dentry);
		/* Make the changes of the rename persistent */
		write_inode_now(old_replica_parent->d_inode, 1);
		write_inode_now(new_replica_parent->d_inode, 1);

		if(old_meta_id != -1){
			reset(old_replica_dentry, "trusted.sfs.meta");
		}
#ifdef DEBUG		
		printk(KERN_INFO "rename2non: new_meta_id %d replica_id %d\n", -1, SFS_D(old_dentry)->replica_id);
#endif
		set_meta_id(old_dentry, -1);
	
	}else {
#ifdef DEBUG		
		printk(KERN_INFO "rename2non: can not turn %s %s\n", old_replica_dentry->d_name.name, new_replica_dentry->d_name.name);
#endif
		new_meta_id = select_normal_dir_rr(new_parent);
		
		/* Create the meta file in Partition #new_meta_id */
		new_meta_parent = find_replica_dentry(new_parent, new_meta_id);
		new_meta_dentry = get_replica_dentry(new_meta_parent, new_dentry, new_meta_id);

		mode = old_dentry->d_inode->i_mode;
		mutex_lock_nested(&new_meta_parent->d_inode->i_mutex, I_MUTEX_OTHER_NORMAL);
		err = new_meta_parent->d_inode->i_op->create(new_meta_parent->d_inode, new_meta_dentry, mode, 0);
		mutex_unlock(&new_meta_parent->d_inode->i_mutex);

		if(unlikely(err))
			goto out;
	
		/* Create a pointer link to make it point to the data file of the destination file to be created */	
		pointer[POINTER] = old_replica_id;
		err = put(new_meta_dentry, "trusted.sfs.meta", pointer);
		if(unlikely(err))
			goto out;
	
		/* Sync the changes of the newly created meta file to storage */
		write_inode_now(new_meta_dentry->d_inode, 1);

		/* Create a rename link to make it point back to the newly created meta file */
		pointer[POINTER] = -1;	
		pointer[RENAME] = new_meta_id;
		err = put(old_replica_dentry, "trusted.sfs.meta", pointer);
		if(unlikely(err))
			goto out;
			
		write_inode_now(old_replica_dentry->d_inode, 1);	
		
		/* Rename the old data file to the new location in Partition #old_replica_id 
		 * and set xattr("trusted.sfs.data", new_meta_id) to make it point to the meta file
 		 */
		
		sfs_lock_rename(new_replica_parent, old_replica_parent);
		err = old_replica_parent->d_inode->i_op->rename(old_replica_parent->d_inode, old_replica_dentry, 
						new_replica_parent->d_inode, new_replica_dentry);
		sfs_unlock_rename(new_replica_parent, old_replica_parent);
		if(unlikely(err))
			goto out;

		d_move(old_replica_dentry, new_replica_dentry);
		
		/* Make the changes of the rename persistent */
		write_inode_now(old_replica_parent->d_inode, 1);
		write_inode_now(new_replica_parent->d_inode, 1);
		
		pointer[POINTER] = new_meta_id;
		pointer[RENAME] = -1;
		put(old_replica_dentry, "trusted.sfs.meta", pointer);
#ifdef DEBUG	
		printk(KERN_INFO "rename2non: new_meta_id %d replica_id %d\n", new_meta_id, SFS_D(old_dentry)->replica_id);
#endif
		set_meta_id(old_dentry, new_meta_id);
	}

	/*
  	 * If the source file is partitioned, SFS needs to delete the meta file of it at last 
	 */
	if(old_meta_id != -1){
		old_meta_dentry = SFS_D(old_dentry)->replicas[old_meta_id];
		old_meta_parent = old_meta_dentry->d_parent;
		err = simple_unlink_not_put(old_meta_parent->d_inode, old_meta_dentry);
		/* If error happens, SFS will delete it when validating it by lookup */
		if(unlikely(err))
			err = 0;
		
	}


	sfs_d_move(old_dentry, new_dentry, old_replica_id);
out: 
	return err;
}

static int rename2exist(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	int err = 0, can_turn, clone_type, overwritten = 0, old_replica_id, old_meta_id,
		exist_replica_id, exist_meta_id,
		new_meta_id;

	struct dentry *old_parent, *new_parent,
		*old_replica_dentry, *new_replica_dentry, *exist_replica_dentry,
		*old_replica_parent, *new_replica_parent, *exist_replica_parent,
		*exist_meta_parent, *exist_meta_dentry,
		*old_meta_parent, *old_meta_dentry;
	struct dentry *hidden_dentry;
	int hidden_id;

	int pointer[4] = {-1,-1,-1, -1};

	old_parent = old_dentry->d_parent;
	new_parent = new_dentry->d_parent;
	
	old_replica_dentry = list_first_replica_dentry(old_dentry);
	old_replica_id = get_replica_id(old_dentry);
	old_meta_id = get_meta_id(old_dentry);
	if(old_meta_id != -1){
		old_meta_dentry = find_replica_dentry(old_dentry, old_meta_id);
		old_meta_parent = find_replica_dentry(old_parent, old_meta_id);
	}
	
	old_replica_parent = find_replica_dentry(old_parent, old_replica_id);
	
	exist_replica_id = get_replica_id(new_dentry);
	exist_replica_dentry = find_replica_dentry(new_dentry, exist_replica_id);
	exist_replica_parent = find_replica_dentry(new_parent, exist_replica_id);
	exist_meta_id = get_meta_id(new_dentry);
	if(exist_meta_id != -1){
		exist_meta_dentry = find_replica_dentry(new_dentry, exist_meta_id);
		exist_meta_parent = find_replica_dentry(new_parent, exist_meta_id);
	}
	
	can_turn = can_turn2normal(new_parent, old_replica_id);
	if(can_turn)
		clone_type = CLONE_NORMAL;
	else
		clone_type = CLONE_SHADOW;

	new_replica_parent = create_replica_dir(new_parent, old_replica_id, clone_type);	
	if(unlikely(IS_ERR(new_replica_parent))){
		err = PTR_ERR(new_replica_parent);
		goto err_out;
	}
	
	new_replica_dentry = get_replica_dentry(new_replica_parent, new_dentry, old_replica_id);
	if(unlikely(IS_ERR(new_replica_dentry))){
		err = PTR_ERR(new_replica_dentry);
		goto err_out;
	}

	if(can_turn){
	/* Condition 1 */
		
		/*Condition 1.1: The existing destination file resides in the same partition with the source file */
#ifdef DEBUG
		printk(KERN_INFO "rename2exit: can turn %s %s\n", old_replica_dentry->d_name.name, new_replica_dentry->d_name.name);
#endif
		if(exist_replica_id == old_replica_id || exist_meta_id == old_replica_id){
		/* SFS would overwrite the meta file or data file of the existing destination file*/
			overwritten = 1;

			/* The rename link which points to itself is used to mask the pointer link */
			pointer[RENAME] = old_replica_id;
			err = put(old_replica_dentry, "trusted.sfs.meta", pointer);
			if(unlikely(err))
				goto err_out;
				
		}
		/* Condition 1.2: 
		 * SFS should first hide the existing destination file by set xattr("trusted.sfs.hidden", old_replica_id) 
		 */
		else{
			if(exist_meta_id != -1){
			/* Condition 1.2.1: Hide the meta file of the existing destination file */
				hidden_dentry = exist_meta_dentry;		
				hidden_id = exist_meta_id;				
			}else{
			/* Condition 1.2.2: Hide the data file of the existing destination file */
				hidden_dentry = exist_replica_dentry;
				hidden_id = exist_replica_id;
			}

			/* The stale link points to the meta/data file which is hidden by the newly created destination file*/	
			pointer[STALE] = hidden_id;
			err = put(old_replica_dentry, "trusted.sfs.meta", pointer);
			if(unlikely(err))	
				goto err_out;
			
			/* Make the changes persistent */
			write_inode_now(old_replica_dentry->d_inode, 1);
			
			/* The hidden link points to the destination file to be created */
			pointer[HIDDEN] = old_replica_id;
			pointer[STALE] = -1;
			err = put(hidden_dentry, "trusted.sfs.meta", pointer);
			if(unlikely(err)){
				printk(KERN_INFO "rename: set hiddern xattr failed\n");
				goto err_out;
			}
			
			/*Sync the changes to storage right now */
			write_inode_now(hidden_dentry->d_inode, 1);

		}

		sfs_lock_rename(new_replica_parent, old_replica_parent);
		err = old_replica_parent->d_inode->i_op->rename(old_replica_parent->d_inode, old_replica_dentry, 
					new_replica_parent->d_inode, new_replica_dentry);
		sfs_unlock_rename(new_replica_parent, old_replica_parent);
		if(unlikely(err)){
			/*Revert to the initial state */
			if(!overwritten){
				modify(old_replica_dentry, "trusted.sfs.meta", -1, STALE);
				modify(hidden_dentry, "trusted.sfs.meta", -1, HIDDEN);
			}else 
				modify(old_replica_dentry, "trusted.sfs.meta", -1, RENAME);			

			goto err_out;		
		}

		d_move(old_replica_dentry, new_replica_dentry);
#ifdef DEBUG
		printk(KERN_INFO "rename2exist:  can turn new_meta_id %d replica_id %d\n", -1, SFS_D(old_dentry)->replica_id);
#endif
		set_meta_id(old_dentry, -1);	

		/* Make the changes of the rename persistent */
		write_inode_now(old_replica_parent->d_inode, 1);
		write_inode_now(new_replica_parent->d_inode, 1);
	
		if(!overwritten){
			
			/* Delete the meta file of the existing destination file */
			if(exist_meta_id != -1){
				err = simple_unlink_not_put(exist_meta_parent->d_inode, exist_meta_dentry);
				if(unlikely(err)){
					printk(KERN_INFO "Deleting the existing meta file failed during rename %d\n", err);
					err = 0; /* SFS will try to delete it during the next rename */
					goto out;
				}

				write_inode_now(exist_meta_parent->d_inode, 1);	
			}
			/* Delete the data file of the existing destination file */
			err = simple_unlink_not_put(exist_replica_parent->d_inode, exist_replica_dentry);
			if(unlikely(err)){
				printk(KERN_INFO "deleting the existing data file failed during rename %d\n", err);
				err = 0;
				goto out;
			}
		
			/*Make the deletion persistent */
			write_inode_now(exist_replica_parent->d_inode, 1);	

			pointer[STALE] = -1;
			pointer[HIDDEN] = -1;
			put(old_replica_dentry, "trusted.sfs.meta", pointer);
	
		}else {
			/* If the meta file is overwritten, SFS should delete the existing data file; do nothing otherwise*/
			if(old_replica_id == exist_meta_id){
				err = simple_unlink_not_put(exist_replica_parent->d_inode, exist_replica_dentry);
				if(unlikely(err)){
					printk(KERN_INFO "deleting the exsiting data file failed during rename %d\n", err);
					err = 0;
					goto out;
				}
			
				write_inode_now(exist_replica_parent->d_inode, 1);
			}

			pointer[RENAME] = -1;
			put(old_replica_dentry, "trusted.sfs.meta", pointer);

		}			


	}
	/* Condition 2: Need to add a meta file for the new destination file */
	else {

		struct dentry *meta_dentry;	
		/* Condition 2.1: The destination file is partitioned */
#ifdef DEBUG
		printk(KERN_INFO "rename2exit: can not turn %s %s\n", old_replica_dentry->d_name.name, new_replica_dentry->d_name.name);
#endif
		if(exist_meta_id != -1){
		/* Condition 2.1.1: Add a temporary link to the meta file to make it point to the new destination file */

			meta_dentry = exist_meta_dentry;
			new_meta_id = exist_meta_id;
 
		}else{
		/* Condition 2.1.2: Add a temporary link to the data file to make it point to the new destination file */

			meta_dentry = exist_replica_dentry;
			new_meta_id = exist_replica_id;
		}
		
		/* Create a rename link to point to the data file of the destination file to be created*/
		pointer[RENAME] = old_replica_id;
		err = put(meta_dentry, "trusted.sfs.meta", pointer);
		if(unlikely(err)){
			printk(KERN_INFO "rename: set tmp_meta xattr failed\n");
			goto err_out;
		}

		/*Sync the changes of xattr of meta_dentry to storage */
		write_inode_now(meta_dentry->d_inode, 1);

		
		/* Create a rename link to point back to the meta file */
		pointer[RENAME] = new_meta_id;
		err = put(old_replica_dentry, "trusted.sfs.meta", pointer);
		if(unlikely(err)){
			printk(KERN_INFO "rename: set tmp_data xattr failed\n");
			goto err_out;
		}

		/*Sync the changes of xattr of old_replica_dentry to storage */
		write_inode_now(old_replica_dentry->d_inode, 1);
		
		sfs_lock_rename(new_replica_parent, old_replica_parent);
		err = old_replica_parent->d_inode->i_op->rename(old_replica_parent->d_inode, old_replica_dentry, 
					new_replica_parent->d_inode, new_replica_dentry);
		sfs_unlock_rename(new_replica_parent, old_replica_parent);
		if(unlikely(err))
			goto err_out;		
		
		d_move(old_replica_dentry, new_replica_dentry);	
#ifdef DEBUG
		printk(KERN_INFO "rename2exist:  can not turn new_meta_id %d replica_id %d\n", new_meta_id, SFS_D(old_dentry)->replica_id);
#endif	
		set_meta_id(old_dentry, new_meta_id);
	
		/*Make the changes of the rename persistent */
		write_inode_now(old_replica_parent->d_inode, 1);
		write_inode_now(new_replica_parent->d_inode, 1);
		
		/* If the existing destination file is partitioned, SFS should delete the data file of it */
		if(exist_meta_id != -1 && (exist_replica_id != old_replica_id)){
			err = simple_unlink_not_put(exist_replica_parent->d_inode, exist_replica_dentry);
			if(unlikely(err)){
				printk(KERN_INFO "the deletion of the existing data file of the destination file failed %d\n", err); 
				err = 0;
				goto out;
			}

			/* Make the deletion persistent */
			write_inode_now(exist_replica_parent->d_inode, 1);
		}
		
		pointer[STALE] = -1;
		pointer[HIDDEN] = -1;
		pointer[RENAME] = -1;
		pointer[POINTER] = new_meta_id;
		put(old_replica_dentry, "trusted.sfs.meta", pointer);

		pointer[POINTER] = old_replica_id;
		put(meta_dentry, "trusted.sfs.meta", pointer);
		
	}

out:
	/* Cleanup the meta file of the source file if it is partitioned*/
	if(old_meta_id != -1){
		err = simple_unlink_not_put(old_meta_parent->d_inode, old_meta_dentry);
		if(unlikely(err))
			err = 0; /* If the deletion failed, SFS would try to delete it after remount */
			
	}
	sfs_d_move(old_dentry, new_dentry, old_replica_id);

err_out:
	return err;
}

static int sfs_rename_other(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	int err;

	/* SFS should first validate the src file and the dest file to cleanup stale data */
	err = validate_file(old_dentry);
	if(unlikely(err))
		return err;
	
	if(new_dentry->d_inode){
		err = validate_file(new_dentry);
		if(unlikely(err))
			return err;

	}

	if(new_dentry->d_inode)
		err = rename2exist(old_dir, old_dentry, new_dir, new_dentry);
	else
		err = rename2non(old_dir, old_dentry, new_dir, new_dentry);


	return err;
}


int sfs_rename(struct inode *old_dir, struct dentry *old_dentry, 
		struct inode *new_dir, struct dentry *new_dentry)
{
	int error = 0;
	int is_dir = S_ISDIR(old_dentry->d_inode->i_mode);
	if(is_dir)
		error = sfs_rename_dir(old_dir, old_dentry, new_dir, new_dentry);
	else 
		error = sfs_rename_other(old_dir, old_dentry, new_dir, new_dentry);

	return error;
}

