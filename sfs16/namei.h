#ifndef _SFS_NAMEI_H
#define _SFS_NAMEI_H


extern int init_dentrycache(void);
extern void destroy_dentrycache(void);

extern int check_normal_dir(struct dentry *parent, int replica_id);
extern struct sfs_dentry *sfs_dentry_alloc(struct dentry *dentry);
extern void d_add_replica(struct dentry *sfs_master, struct dentry *sfs_replica,
	unsigned int replica_id);
extern struct dentry *create_replica_dir(struct dentry *dentry, int replica_id, int clone_type);
extern int simple_add_dir_entry(struct dentry *dentry, umode_t mode, int replica_id, int clone_type);
extern const struct inode_operations sfs_dir_inode_operations; 
extern int simple_unlink_not_put(struct inode *replica_dir, struct dentry *replica_dentry);
extern int validate_file(struct dentry *dentry);
extern struct dentry* get_replica_dentry(struct dentry *replica_parent, struct dentry *dentry, unsigned int replica_id);
extern int can_turn2normal(struct dentry *dentry, int replica_id);

#endif 
