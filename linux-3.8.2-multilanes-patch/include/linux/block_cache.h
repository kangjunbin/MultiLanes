/*
 * include/linux/block_cache.h
 * Implement the virtualized device driver of MultiLanes based on the loop driver.
 * Copyright (C) 2013-2016 by Junbin Kang <kangjb@act.buaa.edu.cn>, Benlong Zhang <zblgeqian@gmail.com>, Ye Zhai <zhaiye@act.buaa.edu.cn>.
 * Beihang University
 */

 
#include <linux/types.h>
#include <linux/loop.h>


#define LRU_LENGTH 1000 * 1024

struct block_cache_node
{
	sector_t b_blocknr;
	sector_t blk_local;
	struct hlist_node node;
	struct list_head next;
};


static int add_cache(sector_t blknr,sector_t blklocal,struct loop_device *lo);
static int init_cache(struct loop_device *lo);
static struct block_cache_node *find_cache_node(sector_t nr, struct loop_device *lo);
static int free_cache(struct loop_device *lo);

