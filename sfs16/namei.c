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
extern int sfs_dentry_delete(struct dentry *);
extern unsigned int get_replica_count(void);

extern int sfs_rename(struct inode *old_dir, struct dentry *old_dentry, 
		struct inode *new_dir, struct dentry *new_dentry);



static inline void sfs_d_instantiate(struct dentry *dentry, struct inode *inode)
{
	d_instantiate(dentry, inode);
}

int simple_unlink(struct inode *replica_dir, struct dentry *replica_dentry)
{
	int error;

	mutex_lock_nested(&replica_dir->i_mutex, I_MUTEX_OTHER_PARENT);
	mutex_lock_nested(&replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
//	lockdep_off();
	error = replica_dir->i_op->unlink(replica_dir, replica_dentry);
//	lockdep_on();
	if(likely(!error))
		dont_mount(replica_dentry);

	mutex_unlock(&replica_dentry->d_inode->i_mutex);
	mutex_unlock(&replica_dir->i_mutex);
	if(likely(!error)){
		clear_dentry(replica_dentry);
		d_delete(replica_dentry);	
		dput(replica_dentry);
	}
	
	return error;
}
		
int simple_unlink_not_put(struct inode *replica_dir, struct dentry *replica_dentry)
{
	int error;

	mutex_lock_nested(&replica_dir->i_mutex, I_MUTEX_OTHER_PARENT);
	mutex_lock_nested(&replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
//	lockdep_off();
	error = replica_dir->i_op->unlink(replica_dir, replica_dentry);
//	lockdep_on();
	if(likely(!error))
		dont_mount(replica_dentry);

	mutex_unlock(&replica_dentry->d_inode->i_mutex);
	mutex_unlock(&replica_dir->i_mutex);
	if(likely(!error)){
		clear_dentry(replica_dentry);
		d_delete(replica_dentry);	
	}
	
	return error;
}
int check_normal_dir(struct dentry *parent, int replica_id)
{
	int ret = 0;
	struct sfs_sb_info;
	struct dentry *replica_parent;
	struct sfs_dentry *sfs_parent = SFS_D(parent);
	char value[8];
	
	replica_parent = find_replica_dentry(parent, replica_id);
	
	ret = vfs_getxattr(replica_parent, "trusted.sfs.shadow", value, sizeof(value));
	
	
	if(ret > 0 && !strcmp(value, "normal")){
		spin_lock(&sfs_parent->lock);
		if(sfs_parent->normal_dir_count == 0);
			sfs_parent->first_normal_dir_id = replica_id;
		sfs_parent->normal_dir_count++;
		set_bitmap(&sfs_parent->normal_dir_map, replica_id);
		spin_unlock(&sfs_parent->lock);
		ret = 1;
	}else ret = 0;

	return ret;

}

static 
int set_normal_dir(struct dentry *parent, int replica_id)
{
	int error = 0;
	struct dentry *replica_parent;
	struct sfs_dentry *sfs_parent = SFS_D(parent);
	const char value[8] = "normal";	

	if(test_bitmap(sfs_parent->normal_dir_map, replica_id))
		goto out;

	replica_parent = find_replica_dentry(parent, replica_id);
	
	error = vfs_setxattr(replica_parent, "trusted.sfs.shadow", value, sizeof(value), 0);	

	if(likely(!error)){
		spin_lock(&sfs_parent->lock);
		if(sfs_parent->normal_dir_count == 0)
			sfs_parent->first_normal_dir_id = replica_id;
		sfs_parent->normal_dir_count++;
		set_bitmap(&sfs_parent->normal_dir_map, replica_id);		
		spin_unlock(&sfs_parent->lock);

	}


out:
	if(error){
		printk(KERN_INFO "create metadata error %d %s %d\n", error, parent->d_name.name, replica_id);
	}
	return error;
}

int can_turn2normal(struct dentry *dentry, int replica_id)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	int replica_count = get_replica_count(), threshold;

	threshold = SPAN_THRESHOLD > replica_count? replica_count:SPAN_THRESHOLD;
	
	if(!test_bitmap(sfs_dentry->normal_dir_map, replica_id) && sfs_dentry->normal_dir_count >= threshold)
		return 0;
	else 	
		return 1;
}




void d_add_replica(struct dentry *dentry, struct dentry *replica,
	unsigned int replica_id)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	
	if(likely(!sfs_dentry->replicas[replica_id])){
		sfs_dentry->replicas[replica_id] = replica;
	}

}

static inline void clean_replica_dentry(struct dentry *replica)
{
	spin_lock(&replica->d_parent->d_lockref.lock);
	replica->d_parent->d_lockref.count--;
	spin_unlock(&replica->d_parent->d_lockref.lock);
	d_drop(replica);
	list_del(&replica->d_u.d_child);

}
static inline void instantiate_replica_dentry(struct dentry *replica, struct dentry *replica_parent)
{
	replica->d_sb = replica_parent->d_sb;
	spin_lock(&replica_parent->d_lock);
	replica_parent->d_lockref.count++;
	replica->d_parent = replica_parent;
	list_add(&replica->d_u.d_child, &replica_parent->d_subdirs);
	spin_unlock(&replica_parent->d_lock);

}

static inline
struct dentry *sfs_lookup_one_len(const struct qstr *name, struct dentry *replica_parent)
{
	struct dentry *new = NULL, *old = NULL;
	struct inode *base = replica_parent->d_inode;

	new = d_alloc(replica_parent, name);
	old = base->i_op->lookup(base, new, 0);
	if(unlikely(old)){
		dput(new);
		new = old;
	}

	return new;
} 

struct dentry* get_replica_dentry(struct dentry *replica_parent, struct dentry *dentry,
	unsigned int replica_id)
{
	struct dentry *replica_dentry;

#ifdef DEBUG
	printk(KERN_INFO "get replica dentry %s %s \n", replica_parent->d_name.name, dentry->d_name.name);	
#endif
	replica_dentry = find_replica_dentry(dentry, replica_id);
	if(replica_dentry)
		return replica_dentry;
	
	replica_dentry = SFS_D(dentry)->unused_replica_dentry;
	if(replica_dentry){
		clean_replica_dentry(replica_dentry);
		instantiate_replica_dentry(replica_dentry, replica_parent);	
		SFS_D(dentry)->unused_replica_dentry = NULL;	
	}else{

		mutex_lock_nested(&replica_parent->d_inode->i_mutex, I_MUTEX_OTHER_NORMAL);
		replica_dentry = sfs_lookup_one_len(&dentry->d_name, replica_parent);
		mutex_unlock(&replica_parent->d_inode->i_mutex);
		if(unlikely(IS_ERR(replica_dentry))){
			printk(KERN_INFO "is err \n");
			goto out;
		}
	}

	d_add_replica(dentry, replica_dentry, replica_id);		
out:
	return replica_dentry;
}

/**
 * Validate the integrity of partitioned files in SFS
 *  The attrbutes of the meta information
 *  |---POINTER ----|  Take the lowest priority.
 *  |---RENAME  ----|
 *  |---HIDDEN  ----|  
 *  |---STALE   ----|  Take the highest priority.
 */
static int validate_partitioned_file(struct dentry *dentry, int local_id)
{
	int ret, err = 0, remote_id;
	struct dentry *parent, *local_dentry, *remote_dentry, *local_parent, *remote_parent;
	int pointer[4] = {-1, -1, -1, -1}, data_pointer[4] = {-1, -1, -1, -1}/*For data files*/;	
	int stale_id;
	struct dentry *stale_parent, *stale_dentry;

	parent = dentry->d_parent;
	local_dentry = find_replica_dentry(dentry, local_id);
	local_parent = find_replica_dentry(parent, local_id);


	ret = get(local_dentry, "trusted.sfs.meta", pointer); 	
	if(ret > 0){
	/* Condition 0: There exists a stale file hidden by it, SFS should delete the stale file */
		if(pointer[STALE] >= 0){
			stale_id = pointer[STALE];
			stale_parent = find_replica_dentry(parent, stale_id);
			stale_dentry = get_replica_dentry(stale_parent, dentry, stale_id);
			if(stale_dentry->d_inode){
				ret = get(stale_dentry, "trusted.sfs.meta", data_pointer);
				if(ret > 0){
					remote_id = data_pointer[POINTER];
					if(remote_id != -1){
						remote_parent = find_replica_dentry(parent, remote_id);
						remote_dentry = get_replica_dentry(remote_parent, dentry, remote_id);
						if(remote_dentry->d_inode){
							err = simple_unlink_not_put(remote_parent->d_inode, remote_dentry);
							if(unlikely(err))
								goto out;
							
							/* Make the deletion persistent */
							write_inode_now(remote_parent->d_inode, 1);
						}
					}
				}
				
				err = simple_unlink_not_put(stale_parent->d_inode, stale_dentry);
				if(unlikely(err))
					goto out;
				
				/* Make the deletion persistent */
				write_inode_now(stale_parent->d_inode, 1);
			}
			
			/* After rename, it becomes a normal data file which is not partitioned.
			 * We just reset all.	
			 */	
			err = reset(local_dentry, "trusted.sfs.meta");
			if(unlikely(err))
				goto out;
	
			/* The file can not be a partitioned file */
			set_replica_id(dentry, local_id);
			goto out;
		}
	/* Condition 1: The file is hidden, SFS should delete it */
		
		if(pointer[HIDDEN] >= 0){
			remote_id = pointer[HIDDEN];
			remote_parent = find_replica_dentry(parent, remote_id);
			remote_dentry = get_replica_dentry(remote_parent, dentry, remote_id);
			if(remote_dentry->d_inode){
			/* Condition 1.1: The remote file is in place correctly. */
				/* Cleanup the stale data file if it exists*/
				stale_id = pointer[POINTER];
				if(stale_id != -1){
					stale_parent = find_replica_dentry(parent, stale_id);
					stale_dentry = get_replica_dentry(stale_parent, dentry, stale_id);
					if(stale_dentry->d_inode){
						err = simple_unlink_not_put(stale_parent->d_inode, stale_dentry);
						if(unlikely(err)){
							printk(KERN_INFO "unlink a stale data file failed, errno %d\n",err);
							goto out;
						}
					}
				}
			
				err = simple_unlink_not_put(local_parent->d_inode, local_dentry); 
				if(unlikely(err)){
					printk(KERN_INFO "unlink a hidden meta file failed, errno %d\n", err);
					goto out;
				}
				write_inode_now(local_parent->d_inode, 1);

				/* After rename, the remote object is a normal data file which is not partitioned. 
				 * We just reset all.
   				 */	
				err = reset(remote_dentry, "trusted.sfs.meta");
				if(unlikely(err))
					goto out;

				d_add_replica(dentry, remote_dentry, remote_id);	
				set_replica_id(dentry, remote_id);
				goto out;
			}
			/* Condition 1.2: The remote file is missing, SFS corrects the hidden entry */
			else {
				err = modify(local_dentry, "trusted.sfs.meta", -1, HIDDEN);
				if(unlikely(err)){
					printk(KERN_INFO "correct a hidden file failed, errno %d\n", err);
					goto out;
				}
				
				set_replica_id(dentry, local_id);
				/* We do not return right now as it may be a partitioned file */
			}
		}
	/*Condition 2: The file is used as a meta file to point to another location */
		if(pointer[RENAME] >= 0){
			remote_id = pointer[RENAME];
			/* In the case of that the rename link points to itself, 
			 * SFS just removes all the links of it as the rename has succeeded*/
			if(remote_id == local_id){
				/* After rename, the file becomes a normal file which is not partitioned.
				 * We just reset all.
				 */
				err = reset(local_dentry, "trusted.sfs.meta");
				if(unlikely(err))
					goto out;

				set_replica_id(dentry, local_id);
				goto out;
			}

			remote_parent = find_replica_dentry(parent, remote_id);
			remote_dentry = get_replica_dentry(remote_parent, dentry, remote_id);
			if(remote_dentry->d_inode){
			/*Condition 2.1: The remote file is in place correctly. */
				/* Cleanup the stale data file */ 
				
				if(pointer[POINTER] >= 0){
					stale_id = pointer[POINTER];
					stale_parent = find_replica_dentry(parent, stale_id);
					stale_dentry = get_replica_dentry(stale_parent, dentry, stale_id);
					if(stale_dentry->d_inode){
						err = simple_unlink_not_put(stale_parent->d_inode, stale_dentry);
						if(unlikely(err)){
							printk(KERN_INFO "unlink a stale file failed, errno %d\n",err);
							goto out;
						}
					}
				}
				
				pointer[POINTER] = remote_id;
				pointer[HIDDEN] = -1;
				pointer[RENAME] = -1;
				pointer[STALE] = -1;
				err = put(local_dentry, "trusted.sfs.meta", pointer);
				if(unlikely(err))
					goto out;
				
				
				d_add_replica(dentry, remote_dentry, remote_id);
				set_replica_id(dentry, remote_id);
				set_meta_id(dentry, local_id);
				goto out;

			}else {
			/* Condition 2.2: The remote file is missing, the rename operation is unfinished*/
				err = modify(local_dentry, "trusted.sfs.meta", -1, RENAME);
				if(unlikely(err))
					goto out;				
			}
		}
	/* Condition 3: Normal condition. */
			
		if(pointer[POINTER] >= 0){
		/* Condition 3.1: It is a partitioned file */

			remote_id = pointer[POINTER];
			remote_parent = find_replica_dentry(parent, remote_id);
			remote_dentry = get_replica_dentry(remote_parent, dentry, remote_id);

			if(!remote_dentry->d_inode){
			/**
			 * delete the invalid local entry as missing the remote entry
			 */ 
				printk(KERN_INFO "remote dentry is missing\n");

				err = simple_unlink_not_put(local_parent->d_inode,local_dentry); 	

			} else{
				ret = get(remote_dentry, "trusted.sfs.meta", data_pointer);
#ifdef DEBUG			
				printk(KERN_INFO "validate partitioned file %s local_id %d remote_id %d\n", 
								dentry->d_name.name, data_pointer[POINTER], remote_id);
#endif
				if(ret <= 0 || data_pointer[POINTER] != local_id){
					data_pointer[POINTER] = local_id;
					data_pointer[RENAME] = -1;
					data_pointer[HIDDEN] = -1;
					data_pointer[STALE] = -1;
					err = put(remote_dentry, "trusted.sfs.meta", data_pointer);
					if(unlikely(err))
						goto out;
				}

				d_add_replica(dentry, remote_dentry, remote_id);
				set_replica_id(dentry, remote_id); 
				set_meta_id(dentry, local_id);
			}
		}
		/*Condition 3.2: It is a data file */
		else 
			set_replica_id(dentry, local_id);
	}else
		/* The xattr of the file is empty, indicating that it is a data file */
		set_replica_id(dentry, local_id); 

	
	
out:
	return err;

}

/* This function is used by rename and unlink for cleaning up stale data
 *
 */

int validate_file(struct dentry *dentry)
{
	int meta_id;
	
	if(SFS_D(dentry)->meta_id != -1)
		meta_id = SFS_D(dentry)->meta_id;
	else
		meta_id = SFS_D(dentry)->replica_id;
	
	return validate_partitioned_file(dentry, meta_id);
}


static struct dentry *sfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	
	int err, found = 0, fullscan = 0, loop, replica_count = get_replica_count();
	umode_t mode;
	struct dentry *parent, *replica_parent, *new = NULL, *old = NULL, *ret;
	struct sfs_dentry *sfs_dentry;
	struct inode *inode = NULL, *base;
	
	parent = dentry->d_parent;
	/** 
	 * The replica dentry list is already protected by the mutex of the inode.
	 * The concurrently appending operations may happen only when cloning a branch from
	 * the other fs, hence the deleting operations will not happen. 
	 */

//	printk(KERN_INFO " look up parent %s dentry %s\n", parent->d_name.name, dentry->d_name.name);	
	sfs_dentry = sfs_dentry_alloc(dentry);
	if(unlikely(IS_ERR(sfs_dentry))){
		ret = ERR_PTR(-ENOMEM);
		goto err_out;
	}


	for(loop = 0; loop < replica_count; loop++){

#ifdef DEBUG
		printk(KERN_INFO "loop %d\n", loop);
#endif

		if(!fullscan && !test_bitmap(SFS_D(parent)->normal_dir_map, loop))
			continue;
		
		replica_parent = find_replica_dentry(parent, loop);
		if(!replica_parent)
			continue;
	
		base = replica_parent->d_inode;
		if(!base)
			continue;

		if(SFS_D(dentry)->replicas[loop])
			continue;

		/** 
		 * We have been nested in vfs lookup with i_mutex holding
		 */

		mutex_lock_nested(&base->i_mutex,I_MUTEX_OTHER_NORMAL);
		if(new == NULL)
			new = d_alloc(replica_parent, &dentry->d_name);
		else {
			/* Cleanup the new dentry */
			clean_replica_dentry(new);
			instantiate_replica_dentry(new, replica_parent);
		}	
			
		old = base->i_op->lookup(base, new, flags);

		if(unlikely(old)){
			dput(new);
			new = old;
		}

/*
		name = &dentry->d_name;
		new = sfs_lookup_one_len(name->name, replica_parent, name->len);
*/		
		mutex_unlock(&base->i_mutex);
		if(unlikely(IS_ERR(new))){
			ret = new;
			goto err_out;
		}

		if(new->d_inode){	
			
			d_add_replica(dentry, new, loop);
#ifdef DEBUG
			printk(KERN_INFO "found new %s\n", new->d_name.name);
#endif
			if(!found)
				found = 1;

			mode = new->d_inode->i_mode;
			if(!S_ISDIR(mode)){
				int err;
			 
				err = validate_partitioned_file(dentry, loop);	
				if(unlikely(err)){
					ret = ERR_PTR(err);
					clean_dentry(dentry);
					goto err_out;
				}
				
				if(SFS_D(dentry)->replica_id == -1)
					found = 0;
				
				new = NULL;	
				goto out;

			}else {
 				if(unlikely((err = check_normal_dir(dentry, loop)) < 0)){
					ret = ERR_PTR(err);
					printk(KERN_INFO "check normal directory err %d\n", err);
					clean_dentry(dentry);
					goto err_out;
				}

				if(!fullscan){

					fullscan = 1;
					loop = -1;
				}
				

			}
			new = NULL;

		}		
	}
out:
	if(new)
		dput(new);

	if(found){
		inode = sfs_new_inode(dentry->d_sb, mode);
		if(unlikely(IS_ERR(inode))){
			ret = (void *)inode;
			printk(KERN_INFO "lookup: alloc new inode failed\n");	
			goto err_out;
		}
	}
	
	
	
	ret = d_splice_alias(inode, dentry);
	return ret;

err_out:
	return ret;

}




/**
 * sfs_clone_branch: create a tree branch in the replica fs
 * @sfs_dentry: The leaf of the tree branch in sfs 
 * @replica_id: The replica id of the replica fs to which a branch will be cloned
 *
 * we traverse and record the path from current dentry to the root dentry 
 * and create the same path in the special replica fs.
 */


struct dentry *sfs_clone_branch(struct dentry *dentry, unsigned int replica_id)
{
	struct path_node {
		struct list_head list;
		struct dentry *dentry;
	};
	int err;
	struct list_head path_list;
	struct dentry *parent, *tmp, *replica_dentry = ERR_PTR(-EIO), *found;
	struct path_node *node, *next;
	struct inode *dir;
	umode_t mode;
	INIT_LIST_HEAD(&path_list);
	tmp = dentry;
	for(;;tmp = tmp->d_parent){
		node = kmalloc(sizeof(struct path_node),GFP_KERNEL);
		if(unlikely(!node)){
			replica_dentry = ERR_PTR(-ENOMEM);
			goto out;
		}

//	printk(KERN_INFO "clone  %s\n", tmp->d_name.name);
		found = find_replica_dentry(tmp,replica_id);
		if(!found){
			
			INIT_LIST_HEAD(&node->list);
			node->dentry = tmp;
			list_add(&node->list, &path_list);
			continue;
	
		}else {
		
			if(found->d_inode)
				break;
		
			INIT_LIST_HEAD(&node->list);
			node->dentry = tmp;
			list_add(&node->list, &path_list);
		}
		
		if(tmp == SFS_S(tmp->d_sb)->s_data_root)
			break;
	}
	
	if(unlikely(!found)){
		replica_dentry = ERR_PTR(-EIO);
		goto out;
	}
	
	tmp = found;	
	list_for_each_entry(node, &path_list, list){

	
		dir = tmp->d_inode;
#ifdef DEBUG	
		printk(KERN_INFO "clone parent %s dentry %s \n", tmp->d_name.name, node->dentry->d_name.name);
#endif
		replica_dentry = get_replica_dentry(tmp, node->dentry, replica_id);
		if(unlikely(IS_ERR(replica_dentry))){
			goto out;
		}
		
		mode = node->dentry->d_inode->i_mode;
	//	printk(KERN_INFO "clone %s  %s %s %d\n", tmp->d_name.name, node->dentry->d_name.name, replica_dentry->d_name.name, replica_id);		
		if(replica_dentry->d_inode){
			tmp = replica_dentry;
			continue;
		}

		mutex_lock_nested(&dir->i_mutex, I_MUTEX_OTHER_NORMAL);
		err = dir->i_op->mkdir(dir, replica_dentry, mode);
		mutex_unlock(&dir->i_mutex);
		if(unlikely(err)){
			printk(KERN_INFO "clone mkdir failed %d\n",err);
			replica_dentry = ERR_PTR(err);
			goto out;
		}

		tmp = replica_dentry;
	}
	

out:
	list_for_each_entry_safe(node, next, &path_list, list){
		list_del(&node->list);
		kfree(node);
	}
	return replica_dentry;
}


/**
 * add new entries (regular files, directories, symlinks, and device nodes) to the sfs
 */



enum op_type{
	Creat,
	Mkdir,
	Symlink,
	Mknod,
	Link,
	Rename
};

struct add_arg {
	enum op_type type;
	umode_t mode;
	union {
		struct {
			bool excl;
		}creat;
		struct {
			const char *oldname;
		}symlink;
		struct {
			dev_t dev;
		}mknod;
		struct {
			struct dentry *old_replica_dentry;
		}link;
		
		}u;
};

struct dentry *create_replica_dir(struct dentry *dentry, int replica_id, int clone_type)
{
	struct dentry *replica_dentry;
	int err;
	
	replica_dentry = find_replica_dentry(dentry, replica_id);

	if(!replica_dentry || !replica_dentry->d_inode){
		replica_dentry = sfs_clone_branch(dentry, replica_id);
		if(unlikely(IS_ERR(replica_dentry))){
			printk(KERN_INFO "sfs_add entry: create branch error %ld\n", PTR_ERR(replica_dentry));
			goto out;
		}
	}

	if(clone_type == CLONE_NORMAL){

		err = set_normal_dir(dentry, replica_id);
		if(unlikely(err)){
			replica_dentry = ERR_PTR(err);	
			goto out;
		}
	}
	

out:
	return replica_dentry;
	
}

int simple_add_dir_entry(struct dentry *dentry, umode_t mode, int replica_id, int clone_type)
{
	int err = 0;
	struct dentry *parent, *replica_parent, *new;
	struct inode *replica_dir;

	parent = dentry->d_parent;
	replica_parent = create_replica_dir(parent, replica_id, clone_type);
	if(IS_ERR(replica_parent)){
		err = PTR_ERR(replica_parent);
		goto out;
	}
	replica_dir = replica_parent->d_inode;
	if(!replica_dir->i_op->mkdir){
		err = -EACCES;
		goto out;
	}

	
	new = find_replica_dentry(dentry, replica_id);
	if(new && new->d_inode)
		goto out;


	new = get_replica_dentry(replica_parent, dentry, replica_id);
	if(unlikely(IS_ERR(new))){
		err = PTR_ERR(new);
		printk(KERN_INFO "simple_add_dir_entry: alloc failed %d\n", err);
		goto out;
	}


	mutex_lock_nested(&replica_dir->i_mutex, I_MUTEX_OTHER_NORMAL);
	err = replica_dir->i_op->mkdir(replica_dir, new, mode);
	mutex_unlock(&replica_dir->i_mutex);
out:
	return err;
}

static int sfs_add_dir_entry(struct inode *dir, struct dentry *dentry, struct add_arg *arg)
{
	int err = 0;
	struct dentry *parent;
	struct inode *inode;
	unsigned int dispatch_id;
	umode_t mode;

	parent = dentry->d_parent;
	if(SFS_D(parent)->normal_dir_count == 0){
		dispatch_id = dispatch();
	}else 
		dispatch_id = SFS_D(parent)->first_normal_dir_id;


	mode = arg->mode | S_IFDIR;
	
	inode = sfs_new_inode(dentry->d_sb, mode);
	if(unlikely(IS_ERR(inode))){
		err = PTR_ERR(inode);
		printk(KERN_INFO "sfs_add_entry: alloc new inode failed\n");
		goto out;
	}
	
	err = simple_add_dir_entry(dentry, mode, dispatch_id, CLONE_NORMAL);

	if(likely(!err)){

		inc_nlink(dir);	
		d_instantiate(dentry, inode);
		
	}else {
		clear_nlink(inode);
		iput(inode);
	}
out:
	return err;
}

/*
static inline
int is_metadata_exist(struct dentry *dentry, int metadata_id)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	int loop, is_exist = 0;
	
	is_exist = sfs_dentry->metadata_id[metadata_id];
	
	return is_exist;
}
*/




int simple_add_partitioned_entry(struct dentry *dentry, struct add_arg *arg, int local_id, int remote_id)
{
	int err = -EIO;
	umode_t mode = arg->mode;
	struct dentry *local_parent, *remote_parent, *local_new, *remote_new, *parent = dentry->d_parent;
	struct inode *local_dir, *remote_dir;
	int pointer[4] = {-1,-1,-1,-1};

#ifdef DEBUG
	printk(KERN_INFO "add partitioned file %s local_id %d remote_id %d\n", 
					dentry->d_name.name, local_id, remote_id);
#endif
	local_parent = create_replica_dir(parent, local_id, CLONE_NORMAL);
	if(IS_ERR(local_parent)){
		err = PTR_ERR(local_parent);
		goto out;
	}
	
	remote_parent = create_replica_dir(parent, remote_id, CLONE_SHADOW);
	if(IS_ERR(remote_parent)){
		err = PTR_ERR(remote_parent);
		goto out;
	}

	local_dir = local_parent->d_inode;
	remote_dir = remote_parent->d_inode;
	
	local_new = get_replica_dentry(local_parent, dentry, local_id);
	if(unlikely(IS_ERR(local_new))){
		err = PTR_ERR(local_new);
		goto out;
	}

	remote_new = get_replica_dentry(remote_parent, dentry, remote_id);
	if(unlikely(IS_ERR(local_new))){
		err = PTR_ERR(remote_new);
		goto out;
	}

	switch(arg->type){
	case Creat:

		mutex_lock_nested(&local_dir->i_mutex, I_MUTEX_OTHER_NORMAL);
		err = local_dir->i_op->create(local_dir, local_new, mode, arg->u.creat.excl);
		mutex_unlock(&local_dir->i_mutex);
		if(unlikely(err))
			break;
		
		pointer[POINTER] = remote_id;
	        err = put(local_new, "trusted.sfs.meta", pointer);
		if(unlikely(err))
			break;

		mutex_lock_nested(&remote_dir->i_mutex, I_MUTEX_OTHER_NORMAL);		
		err = remote_dir->i_op->create(remote_dir, remote_new, mode, arg->u.creat.excl);
		mutex_unlock(&remote_dir->i_mutex);
		if(unlikely(err))
			break;
	
		pointer[POINTER] = local_id;
	        err = put(remote_new, "trusted.sfs.meta", pointer);
		if(unlikely(err))
			break;
		
		break;

	case Link:

		mutex_lock_nested(&local_dir->i_mutex, I_MUTEX_OTHER_NORMAL);
		err = local_dir->i_op->create(local_dir, local_new, mode, 0);
		mutex_unlock(&local_dir->i_mutex);
		if(unlikely(err))
			break;

		pointer[POINTER] = remote_id;
	        err = put(local_new, "trusted.sfs.meta", pointer);
		if(unlikely(err))
			break;

		mutex_lock_nested(&(arg->u.link.old_replica_dentry->d_inode->i_mutex), I_MUTEX_OTHER_CHILD);
		err = remote_dir->i_op->link(arg->u.link.old_replica_dentry, remote_dir, remote_new);
		mutex_unlock(&(arg->u.link.old_replica_dentry->d_inode->i_mutex));
		if(unlikely(err))
			break;
	
		pointer[POINTER] = local_id;
	        err = put(remote_new, "trusted.sfs.meta", pointer);
		if(unlikely(err))
			break;
		
		break;

			
	default:
		break;	


	}

out:
#ifdef DEBUG
	printk(KERN_INFO "partitioned entry add finished \n");
#endif
	return err;

}
 
int simple_add_entry(struct dentry *dentry, struct add_arg *arg, int local_id)
{
	int err = -EIO;
	umode_t mode = arg->mode;
	struct inode * replica_dir;	
	struct dentry *new, *replica_parent, *parent = dentry->d_parent;

#ifdef DEBUG
	printk(KERN_INFO "add simple entry %s id %d\n", dentry->d_name.name, local_id);
#endif
	replica_parent = create_replica_dir(parent, local_id, CLONE_NORMAL);
	if(IS_ERR(replica_parent)){
		err = PTR_ERR(replica_parent);
		goto out;
	}

	replica_dir = replica_parent->d_inode;


	new = get_replica_dentry(replica_parent, dentry, local_id);

	//printk(KERN_INFO "add new entry %s %s\n", replica_parent->d_name.name, new->d_name.name, metadata_id); 
	if(unlikely(IS_ERR(new))){
		err = PTR_ERR(new);
		printk(KERN_INFO "sfs_add_entry: alloc failed %d\n", err);
		goto out;
	}

	mutex_lock_nested(&replica_dir->i_mutex, I_MUTEX_OTHER_NORMAL);
	switch(arg->type){
	case Creat:
		if(!replica_dir->i_op->create){
			err = -EACCES;
			break;
		}
		err = replica_dir->i_op->create(replica_dir, new, mode, arg->u.creat.excl);
		
		break;
	case Mkdir:
		if(!replica_dir->i_op->mkdir){
			err = -EACCES;
			break;
		}
	
		err = replica_dir->i_op->mkdir(replica_dir, new, mode);

		break;
	case Symlink:
		if(!replica_dir->i_op->symlink){
			err = -EACCES;
			break;
		}
		err = replica_dir->i_op->symlink(replica_dir, new, arg->u.symlink.oldname);

		break;
	case Mknod:
		if(!replica_dir->i_op->mknod){
			err = -EACCES;
			break;
		}
		err = replica_dir->i_op->mknod(replica_dir, new, mode, arg->u.mknod.dev);

		break;
	case Link:
		if(!replica_dir->i_op->link){
			err = -EACCES;
			break;
		}
		mutex_lock_nested(&arg->u.link.old_replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
		err = replica_dir->i_op->link(arg->u.link.old_replica_dentry, replica_dir, new);
		mutex_unlock(&arg->u.link.old_replica_dentry->d_inode->i_mutex);
		break;
	default:
		break;

	}

	mutex_unlock(&replica_dir->i_mutex);

out:

	return err;

}

		
static int sfs_add_entry(struct inode *dir, struct dentry *dentry, struct add_arg *arg)
{
	int err = 0;
	umode_t mode = arg->mode;
	struct dentry *parent;
	struct inode *inode;
	unsigned int dispatch_id;
	int partitioned_id = -1;
	
	parent = dentry->d_parent;
	dispatch_id = local_dispatch(parent, &partitioned_id);

	
	inode = sfs_new_inode(dentry->d_sb, mode);
	if(unlikely(IS_ERR(inode))){
		err = PTR_ERR(inode);
		printk(KERN_INFO "sfs_add_entry: alloc new inode failed\n");
		goto out;
	}
	

	/**
	 * Creating a partitioned entry or a normal entry
 	 */

	if(partitioned_id == -1)
		err = simple_add_entry(dentry, arg, dispatch_id);	
	else
		err = simple_add_partitioned_entry(dentry, arg, dispatch_id, partitioned_id);

	
	if(likely(!err)){
		if(partitioned_id == -1)
			set_replica_id(dentry, dispatch_id);	
		else {
			set_replica_id(dentry, partitioned_id);
			set_meta_id(dentry, dispatch_id);
		}
		
		d_instantiate(dentry, inode);
		
	}else {
		clear_nlink(inode);
		iput(inode);
	}

out:
	return err;
}

static int sfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		bool excl)
{
	struct add_arg arg ={
		.type = Creat,
		.mode = mode,
		.u.creat = {
			.excl	= excl
		}
	};
	return sfs_add_entry(dir, dentry, &arg);	
}

static int sfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct add_arg arg ={
		.type = Mkdir,
		.mode = mode,
	};
	return sfs_add_dir_entry(dir, dentry, &arg);	
}

static int sfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	struct add_arg arg ={
		.type = Symlink,
		.u.symlink = {
			.oldname = oldname
		}
	};
	return sfs_add_entry(dir, dentry, &arg);	
}

static int sfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
	dev_t rdev)
{
	struct add_arg arg ={
		.type = Mknod,
		.mode = mode,
		.u.mknod = {
			.dev	= rdev
		}
	};
	return sfs_add_entry(dir, dentry, &arg);	
}

static void sfs_dec_count(struct inode *inode)
{
	if(!S_ISDIR(inode->i_mode) || inode->i_nlink > 2)
		drop_nlink(inode);
}



int sfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int error = -ENOENT, loop, parent_normal_dir_id, replica_count = get_replica_count();
	struct dentry *parent, *replica_parent, *replica_dentry;
	struct inode *replica_dir;

	parent = dentry->d_parent;
	if(SFS_D(parent)->normal_dir_count > 0)
		parent_normal_dir_id = SFS_D(parent)->first_normal_dir_id;
	else 
		parent_normal_dir_id = -1;
//	printk(KERN_INFO "sfs_rmdir master %s hashed ? %d\n",dentry->d_name.name, d_unhashed(dentry));

	for(loop = 0; loop < replica_count; loop++){
		if(loop == parent_normal_dir_id)
			continue;
		
		replica_dentry = find_replica_dentry(dentry, loop);
		if(!replica_dentry || !replica_dentry->d_inode){
			continue;
		}
		replica_parent = replica_dentry->d_parent;
		replica_dir = replica_parent->d_inode;

		mutex_lock_nested(&replica_dir->i_mutex, I_MUTEX_OTHER_PARENT);
		mutex_lock_nested(&replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
//		lockdep_off();
		error =replica_dir->i_op->rmdir(replica_dir, replica_dentry);
//		lockdep_on();
		mutex_unlock(&replica_dentry->d_inode->i_mutex);
		mutex_unlock(&replica_dir->i_mutex);

		if(unlikely(error))
			goto err_out;
	

		replica_dentry->d_inode->i_flags |= S_DEAD;
		dont_mount(replica_dentry);
		clear_dentry(replica_dentry);
		d_delete(replica_dentry);
	
		if(unlikely(error))
			goto err_out;
	}

	if(parent_normal_dir_id < 0)
		goto out;

	/**
	 * remove the last directory residing under a certain normal parent directory
	 */	
	replica_dentry = find_replica_dentry(dentry, parent_normal_dir_id);
	if(!replica_dentry || !replica_dentry->d_inode){
			error = -EIO;
			goto err_out;
	}
	replica_parent = replica_dentry->d_parent;
	replica_dir = replica_parent->d_inode;
	
	mutex_lock_nested(&replica_dir->i_mutex, I_MUTEX_OTHER_PARENT);
	mutex_lock_nested(&replica_dentry->d_inode->i_mutex, I_MUTEX_OTHER_CHILD);
	error =replica_dir->i_op->rmdir(replica_dir, replica_dentry);
	mutex_unlock(&replica_dentry->d_inode->i_mutex);
	mutex_unlock(&replica_dir->i_mutex);


	if(unlikely(error))
		goto err_out;


	replica_dentry->d_inode->i_flags |= S_DEAD;
	dont_mount(replica_dentry);
	clear_dentry(replica_dentry);
	d_delete(replica_dentry);

	if(unlikely(error))
		goto err_out;

out:
//	sfs_dec_count(dir);	
	clear_nlink(dentry->d_inode);	
	clean_dentry(dentry);
	return error;

err_out:
	return error;
}




static int delete_partitioned_file(struct inode *dir, struct dentry *dentry)
{
	int error;
	struct dentry *local_parent, *remote_parent, *local, *remote;
	struct inode *local_dir, *remote_dir;
	unsigned int replica_id, meta_id;

	replica_id = SFS_D(dentry)->replica_id;
	meta_id = SFS_D(dentry)->meta_id;
	local = find_replica_dentry(dentry, meta_id);
	remote = find_replica_dentry(dentry, replica_id);
	local_parent = local->d_parent;
	remote_parent = remote->d_parent;
	local_dir = local_parent->d_inode;
	remote_dir = remote_parent->d_inode;

	/**
	 * delete the remote entry first and then remove the local entry.
	 */
#ifdef DEBUG
	printk(KERN_INFO "delete the partitoned file %s local_id %d remote_id %d\n", remote->d_name.name, meta_id, replica_id);
#endif
	error = simple_unlink_not_put(remote_dir, remote);
	if(unlikely(error))
		goto err_out;
	
	error = simple_unlink_not_put(local_dir, local);
	if(unlikely(error))
		goto err_out;

err_out:
	return error;

}
static int sfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int error;
	struct dentry *replica_parent, *replica_dentry;
	struct inode *replica_dir;
	unsigned int replica_id;

	/* If there are stale data, SFS should first clean them up */

	error = validate_file(dentry);
	if(unlikely(error))
		return error;

	if(SFS_D(dentry)->meta_id != -1){
		error = delete_partitioned_file(dir, dentry);
		goto out;
	}
	
	replica_dentry = list_first_replica_dentry(dentry);
	replica_id = get_replica_id(dentry);
	replica_parent = replica_dentry->d_parent;

	replica_dir = replica_parent->d_inode;

	error = simple_unlink_not_put(replica_dir, replica_dentry);


out:
	if(likely(!error)){
		drop_nlink(dentry->d_inode);
		clean_dentry(dentry);
		
	}

	return error;
}

//extern int sfs_rename(struct inode *old_dir, struct dentry *old_dentry, 
//		struct inode *new_dir, struct dentry *new_dentry);



static int sfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
	int err;
	struct dentry *old_replica_dentry, *new_parent;
	struct inode *inode;
	unsigned int old_replica_id, new_replica_id,
			new_meta_id = -1;
	struct add_arg arg ={
		.type = Link,
		.u.link = {
			.old_replica_dentry = 0
		}
	};

	old_replica_dentry = list_first_replica_dentry(old_dentry);
	old_replica_id = get_replica_id(old_dentry);
	
	new_parent = new_dentry->d_parent;
	
	new_replica_id = old_replica_id;
	if(can_turn2normal(new_parent, old_replica_id) == 0)
		new_meta_id = select_normal_dir_rr(new_parent);
	
	inode = sfs_new_inode(old_dentry->d_sb, old_dentry->d_inode->i_mode);
	if(unlikely(IS_ERR(inode))){
		err = PTR_ERR(inode);
		goto out;
	}
	
	arg.u.link.old_replica_dentry = old_replica_dentry;
	arg.mode = old_dentry->d_inode->i_mode;

	if(new_meta_id == -1)
		err = simple_add_entry(new_dentry, &arg, new_replica_id);	
	else 
		err = simple_add_partitioned_entry(new_dentry, &arg, new_meta_id, new_replica_id); 


	if(likely(!err)){
			
		d_instantiate(new_dentry, inode);
		if(new_meta_id == -1)
			set_replica_id(new_dentry, new_replica_id);
		else {
			set_replica_id(new_dentry, new_replica_id);
			set_meta_id(new_dentry, new_meta_id);
		}
	}
	else
		iput(inode); 
	
	
out:
	return err;
}
const struct inode_operations sfs_dir_inode_operations = {
	.create		= sfs_create,
	.lookup		= sfs_lookup,
	.link		= sfs_link,
	.unlink		= sfs_unlink,
	.symlink	= sfs_symlink,
	.mkdir		= sfs_mkdir,
	.rmdir		= sfs_rmdir,
	.mknod		= sfs_mknod,
	.rename		= sfs_rename,
	.setattr	= sfs_setattr,
	.fiemap		= sfs_fiemap,
};


