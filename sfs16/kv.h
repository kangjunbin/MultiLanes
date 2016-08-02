#ifndef _KV_H
#define _KV_H

#define POINTER 0
#define RENAME 1 
#define HIDDEN 2
#define STALE 3

extern int reset(struct dentry *dentry, const char *key);
extern int modify(struct dentry *dentry, const char *key, const int value, int sequence);

extern int put(struct dentry *dentry, const char *key, const int value[]);

extern int get(struct dentry *dentry, const char *key, int value[]);

#endif
