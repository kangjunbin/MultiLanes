/*
 * include/linux/loop.h
 *
 * Written by Theodore Ts'o, 3/29/93.
 *
 * Copyright 1993 by Theodore Ts'o.  Redistribution of this file is
 * permitted under the GNU General Public License.
 *
 * Implement the virtualized device driver of MultiLanes based on the loop driver.
 * Copyright (C) 2013-2016 by Junbin Kang <kangjb@act.buaa.edu.cn>, Benlong Zhang <zblgeqian@gmail.com>, Ye Zhai <zhaiye@act.buaa.edu.cn>.
 * Beihang University.
 */
#ifndef _LINUX_LOOP_H
#define _LINUX_LOOP_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <uapi/linux/loop.h>
#include <linux/types.h>

/* Possible states of device */
enum {
	Lo_unbound,
	Lo_bound,
	Lo_rundown,
};

struct loop_func_table;
/*
struct blk_req
{
	unsigned int bv_len;
	unsigned long bi_rw;
	sector_t blk_local;
};
*/
struct cio_req {
	int create;
	//int slice;
	//int blks[10];
	sector_t blk_local;
	sector_t blk_phy;
//	sector_t sect_nr_local;
	int blk_num;
	int req_num;
	int error;
//	int off_set;
//	int bv_len;
	struct completion allocated;
	struct list_head list;
};

#define LOOP_DIRTY 1

#define	BLOCK_COUNT	4000 * 1024 / 4
#define CACHE_COUNT 	40000
struct loop_device {
	int		lo_number;
	int		lo_refcnt;
	loff_t		lo_offset;
	loff_t		lo_sizelimit;
	int		lo_flags;
	int		(*transfer)(struct loop_device *, int cmd,
				    struct page *raw_page, unsigned raw_off,
				    struct page *loop_page, unsigned loop_off,
				    int size, sector_t real_block);
	char		lo_file_name[LO_NAME_SIZE];
	char		lo_crypt_name[LO_NAME_SIZE];
	char		lo_encrypt_key[LO_KEY_SIZE];
	int		lo_encrypt_key_size;
	struct loop_func_table *lo_encryption;
	__u32           lo_init[2];
	kuid_t		lo_key_owner;	/* Who set the key */
	int		(*ioctl)(struct loop_device *, int cmd, 
				 unsigned long arg); 

	struct file *	lo_backing_file;
	//added by SVSS 
	//unsigned int 	block_table[BLOCK_COUNT];
	struct hlist_head block_cache[CACHE_COUNT];
	spinlock_t	cache_lock[CACHE_COUNT];
	//struct list_head blk_req_list;
	atomic_t lru_count;
	spinlock_t	lru_lock;
	struct list_head lru;
	//int* block_table;
	spinlock_t	table_lock;
	struct list_head	lo_cio_list;
	spinlock_t		lo_cio_lock;

	struct block_device *lo_device;
	unsigned	lo_blocksize;
	void		*key_data; 

	gfp_t		old_gfp_mask;

	spinlock_t		lo_lock;
	struct bio_list		lo_bio_list;
	unsigned int		lo_bio_count;

	int			lo_state;
	struct mutex		lo_ctl_mutex;
	struct task_struct	*lo_thread,*lo_thread1,*lo_thread2,*lo_thread3;
	wait_queue_head_t	lo_event;
	/* wait queue for incoming requests */
	wait_queue_head_t	lo_req_wait;

	struct request_queue	*lo_queue;
	struct gendisk		*lo_disk;

	unsigned long lo_dirty_state;
};

/* Support for loadable transfer modules */
struct loop_func_table {
	int number;	/* filter type */ 
	int (*transfer)(struct loop_device *lo, int cmd,
			struct page *raw_page, unsigned raw_off,
			struct page *loop_page, unsigned loop_off,
			int size, sector_t real_block);
	int (*init)(struct loop_device *, const struct loop_info64 *); 
	/* release is called from loop_unregister_transfer or clr_fd */
	int (*release)(struct loop_device *); 
	int (*ioctl)(struct loop_device *, int cmd, unsigned long arg);
	struct module *owner;
}; 

int loop_register_transfer(struct loop_func_table *funcs);
int loop_unregister_transfer(int number); 

#endif
