/** 
 * Copyright (C) 2013 Junbin Kang. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include "sfs_fs.h"
#define MAX 1024
struct cpu2fs_mapping {
	struct sfs_sb_info *sbi; 
};

static struct cpu2fs_mapping **cpu2fs_mapping;
static int nr_online_cpus;
static atomic_t last_dispatch;
extern unsigned int get_replica_count(void);

unsigned int dispatch(void)
{
	unsigned int replica_id;

	if(get_replica_count() == 1)
		return 0;

	replica_id = atomic_add_return(1,&last_dispatch);
	if(replica_id > MAX)
		atomic_set(&last_dispatch, 0);

	return (replica_id - 1)% get_replica_count();	
}
/**
 * Select a normal directory in a round-rabin manner with a local counter
 */
int select_normal_dir_rr(struct dentry *dentry)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	int ret, replica_count = get_replica_count();
	
	if(sfs_dentry->normal_dir_count == 0)
		return -1;

	while(!test_bitmap(sfs_dentry->normal_dir_map, sfs_dentry->local_counter)){
		sfs_dentry->local_counter = (sfs_dentry->local_counter + 1) % replica_count;
	}
	
	ret = sfs_dentry->local_counter;
	sfs_dentry->local_counter = (sfs_dentry->local_counter + 1) % replica_count;
	return ret;
}

int local_dispatch(struct dentry *dentry, int *partitioned_id)
{
	struct sfs_dentry *sfs_dentry = SFS_D(dentry);
	int ret = 0, replica_count = get_replica_count();
	int span_threshold;

	span_threshold = replica_count > SPAN_THRESHOLD? SPAN_THRESHOLD:replica_count;
	spin_lock(&sfs_dentry->lock);

	if(sfs_dentry->normal_dir_count < span_threshold){
		do{
			ret = dispatch();

		}while(test_bitmap(sfs_dentry->normal_dir_map, ret));
		
	}else {
		
#ifdef PARTITIONED		
		/**
		 * Enable partitioned functionality to distribute objects under each single directory across all the partitions 
		 */
		dispatch_id = sfs_dentry->global_counter;
		sfs_dentry->global_counter = (sfs_dentry->global_counter + 1) % replica_count;
		if(test_bitmap(sfs_dentry->normal_dir_map, dispatch_id)){
			ret = dispatch_id;
			goto out;
		}
		*partitioned_id = dispatch_id;
#endif
	
		while(!test_bitmap(sfs_dentry->normal_dir_map, sfs_dentry->local_counter)){
				sfs_dentry->local_counter = (sfs_dentry->local_counter + 1) % replica_count;
		}
	
		ret = sfs_dentry->local_counter;
		sfs_dentry->local_counter = (sfs_dentry->local_counter + 1) % replica_count;
	}	
	

	spin_unlock(&sfs_dentry->lock);
	return ret;

}



int init_cpu2fs_mapping(void)
{
	int err = 0, i;
	
	nr_online_cpus = num_online_cpus();
	cpu2fs_mapping = kmalloc(nr_online_cpus * sizeof(struct cpu2fs_mapping *), GFP_KERNEL);
	if(unlikely(!cpu2fs_mapping)){
		err = -ENOMEM;
		goto out;
	}

	for(i = 0; i < nr_online_cpus; i++){
		cpu2fs_mapping[i] = kmalloc(sizeof(struct fs2cpu_mapping *), GFP_KERNEL);
		if(unlikely(!cpu2fs_mapping[i])){
			err = -ENOMEM;
			goto out;
		}
	}
	atomic_set(&last_dispatch, 0);
out:	
	return err;
}

void destroy_cpu2fs_mapping(void )
{
	int i;
	
	for(i = 0; i < nr_online_cpus; i++)
		kfree(cpu2fs_mapping[i]);
	
	kfree(cpu2fs_mapping);
}

int assign_cpus(struct super_block *sb)
{
	int i;
	struct sfs_sb_info *sbi = SFS_S(sb), *replica_sbi;
	int ratio;
	int s_replica_count;
	
	s_replica_count = atomic_read(&sbi->u.m.replica_count);
	nr_online_cpus = 16;
	printk(KERN_INFO "s_replica_count %d online cpus %d", s_replica_count, nr_online_cpus);
	
	if(s_replica_count > nr_online_cpus)
		return -E2BIG;
	else 
		ratio = (nr_online_cpus + s_replica_count - 1)/ s_replica_count;	

	i = 0;	
	list_for_each_replica(sbi, replica_sbi){
		int count = ratio;
		
		while(count--){
			cpumask_set_cpu(i, &replica_sbi->cpu_set);
			cpu2fs_mapping[i]->sbi = replica_sbi;
			i++;
		}
		printk(KERN_INFO "loop assign cpus %d", i);
	}
	printk(KERN_INFO "assign cpus finished");
	return 0;
}

/**
 * invoked only when holding the spin lock of sbi
 */

static void inc_concurrency(struct sfs_sb_info *sbi, unsigned int cpu)
{
	sbi->nr_accessing_processes_percpu[cpu] ++;

	if(sbi->nr_accessing_processes_percpu[cpu] == 1)
		sbi->nr_accessing_cpus ++;
}

static void dec_cocurrency(struct sfs_sb_info *sbi, unsigned int cpu)
{
	sbi->nr_accessing_processes_percpu[cpu] --;

	if(sbi->nr_accessing_processes_percpu[cpu] == 0)
		sbi->nr_accessing_cpus --;
}


static void check_concurrency(struct sfs_sb_info *sbi, int *needed_migrate)
{
	*needed_migrate = 0;
	return;
/*
	spin_lock(&sbi->lock);
	if(sbi->nr_accessing_cpus > CONCURRENCY_THRESHOLD)
		*needed_migrate = 1;
	spin_unlock(&sbi->lock);
*/
}

/**
 * migrate the update thread to the cpu set which the replica fs is pinned to 
 */ 
void migrate_on_update_1(unsigned int replica_id, struct cpumask *saved_mask, unsigned int *dest_cpu)
{
	struct sfs_sb_info *sbi = get_replica_sbi(replica_id);
	int needed_migrate = 0;
	
	
	/**
	 * migrate or not, decided by the cocurrency degree. 
         */	
	check_concurrency(sbi, &needed_migrate);

	/**
	 * if the thread is not needed to migrate, we only bind it onto the current cpu
         * to avoid load banlance migration during I/O processing. 
	 */
	
	*saved_mask = current->cpus_allowed;
	if(!needed_migrate)
		set_cpus_allowed_ptr(current, cpumask_of(task_cpu(current)));
	else 
		set_cpus_allowed_ptr(current, &sbi->cpu_set);
	
	*dest_cpu = task_cpu(current);

	spin_lock(&sbi->lock);
	inc_concurrency(sbi, *dest_cpu);
	spin_unlock(&sbi->lock);

}

void migrate_on_update(unsigned int replica_id, struct cpumask *saved_mask, unsigned int *dest_cpu)
{
	*dest_cpu = -1;
}
/** 
 * bind the update thread to the cpu set which the replica fs is pinned to 
 */
void bind_on_update_1(unsigned int replica_id, struct cpumask *saved_mask, unsigned int *dest_cpu)
{
	struct sfs_sb_info *sbi = get_replica_sbi(replica_id);
	
	*saved_mask = current->cpus_allowed;
	set_cpus_allowed_ptr(current, &sbi->cpu_set);
	*dest_cpu = task_cpu(current);

	spin_lock(&sbi->lock);
	inc_concurrency(sbi, *dest_cpu);
	spin_unlock(&sbi->lock);	
}

void bind_on_update(unsigned int replica_id, struct cpumask *saved_mask, unsigned int *dest_cpu)
{
	*dest_cpu = -1;
}
/** 
 * migrate the update thread to the default cpu set after update 
 */
void finish_update(unsigned int replica_id, struct cpumask *saved_mask, unsigned int dest_cpu)
{
	struct sfs_sb_info *sbi = get_replica_sbi(replica_id);

	if(dest_cpu != -1){
		set_cpus_allowed_ptr(current, saved_mask);
		spin_lock(&sbi->lock);
		dec_cocurrency(sbi, dest_cpu);
		spin_unlock(&sbi->lock);		
	}

}



