#ifndef _SFS_INODE_H
#define _SFS_INODE_H
#include <linux/fs.h>

extern void init_unused_inode_list(void);

extern void release_unused_inode_list(void);

extern void sfs_put_inode(struct dentry *dentry, struct inode *inode);

extern struct inode *sfs_new_inode(struct super_block *sb, umode_t mode);

extern struct inode *sfs_alloc_inode(struct super_block *sb);

extern int sfs_alloc_root(struct super_block *sb);

extern int sfs_setattr(struct dentry *dentry, struct iattr *attr);

extern int sfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
	struct kstat *stat);

extern int sfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
	__u64 start, __u64 end);
extern const struct inode_operations sfs_symlink_inode_operations;
#endif
