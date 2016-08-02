#ifndef _SFS_DISPATCH_H
#define _SFS_DISPATCH_H

extern void migrate_on_update(unsigned int replica_id, struct cpumask *saved_mask, unsigned int *dest_cpu);

extern void bind_on_update(unsigned int replica_id, struct cpumask *saved_mask, unsigned int *dest_cpu);

extern void finish_update(unsigned int replica_id, struct cpumask *cpumask, unsigned int dest_cpu);

extern unsigned int dispatch(void);

extern int select_normal_dir_rr(struct dentry *dentry);

extern int local_dispatch(struct dentry *dentry, int *partitioned_id);

extern int init_cpu2fs_mapping(void);

extern void destroy_cpu2fs_mapping(void );

extern int assign_cpus(struct super_block *sb);

#endif
