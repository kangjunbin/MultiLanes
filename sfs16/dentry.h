#ifndef _H_DENTRY_H
#define _H_DENTRY_H

extern struct sfs_dentry *sfs_dentry_alloc(struct dentry *dentry);

extern void clean_dentry(struct dentry *dentry);






#endif
