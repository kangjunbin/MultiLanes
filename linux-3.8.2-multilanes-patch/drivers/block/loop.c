/*
 *  linux/drivers/block/loop.c
 *
 *  Written by Theodore Ts'o, 3/29/93
 *
 * Copyright 1993 by Theodore Ts'o.  Redistribution of this file is
 * permitted under the GNU General Public License.
 *
 * DES encryption plus some minor changes by Werner Almesberger, 30-MAY-1993
 * more DES encryption plus IDEA encryption by Nicholas J. Leon, June 20, 1996
 *
 * Modularized and updated for 1.1.16 kernel - Mitch Dsouza 28th May 1994
 * Adapted for 1.3.59 kernel - Andries Brouwer, 1 Feb 1996
 *
 * Fixed do_loop_request() re-entrancy - Vincent.Renardias@waw.com Mar 20, 1997
 *
 * Added devfs support - Richard Gooch <rgooch@atnf.csiro.au> 16-Jan-1998
 *
 * Handle sparse backing files correctly - Kenn Humborg, Jun 28, 1998
 *
 * Loadable modules and other fixes by AK, 1998
 *
 * Make real block number available to downstream transfer functions, enables
 * CBC (and relatives) mode encryption requiring unique IVs per data block.
 * Reed H. Petty, rhp@draper.net
 *
 * Maximum number of loop devices now dynamic via max_loop module parameter.
 * Russell Kroll <rkroll@exploits.org> 19990701
 *
 * Maximum number of loop devices when compiled-in now selectable by passing
 * max_loop=<1-255> to the kernel on boot.
 * Erik I. Bolsø, <eriki@himolde.no>, Oct 31, 1999
 *
 * Completely rewrite request handling to be make_request_fn style and
 * non blocking, pushing work to a helper thread. Lots of fixes from
 * Al Viro too.
 * Jens Axboe <axboe@suse.de>, Nov 2000
 *
 * Support up to 256 loop devices
 * Heinz Mauelshagen <mge@sistina.com>, Feb 2002
 *
 * Support for falling back on the write file operation when the address space
 * operations write_begin is not available on the backing filesystem.
 * Anton Altaparmakov, 16 Feb 2005
 * 
 * Still To Fix:
 * - Advisory locking is ignored here.
 * - Should use an own CAP_* category instead of CAP_SYS_ADMIN
 *
 * Implement the virtualized device driver of MultiLanes based on the Loop driver 
 * Copyright (C) 2013-2015 by Junbin Kang <kangjb@act.buaa.edu.cn>, Benlong Zhang <zblgeqian@gmail.com>, Ye Zhai <zhaiye@act.buaa.edu.cn>
 * Beihang University
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/init.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/loop.h>
#include <linux/compat.h>
#include <linux/suspend.h>
#include <linux/freezer.h>
#include <linux/mutex.h>
#include <linux/writeback.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/splice.h>
#include <linux/sysfs.h>
#include <linux/miscdevice.h>
#include <linux/falloc.h>
#include <linux/cpumask.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/blk_types.h>
#include <linux/list.h>
#include <linux/jbd.h>
#include <linux/time.h>
#include <linux/block_cache.h>
#include <linux/list.h>

MODULE_LICENSE("GPL");

static DEFINE_IDR(loop_index_idr);
static DEFINE_MUTEX(loop_index_mutex);

static int max_part;
static int part_shift;

/*
 * Transfer functions
 */
static int transfer_none(struct loop_device *lo, int cmd,
		struct page *raw_page, unsigned raw_off,
		struct page *loop_page, unsigned loop_off,
		int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page) + raw_off;
	char *loop_buf = kmap_atomic(loop_page) + loop_off;

	if (cmd == READ)
		memcpy(loop_buf, raw_buf, size);
	else
		memcpy(raw_buf, loop_buf, size);

	kunmap_atomic(loop_buf);
	kunmap_atomic(raw_buf);
	cond_resched();
	return 0;
}

static int transfer_xor(struct loop_device *lo, int cmd,
		struct page *raw_page, unsigned raw_off,
		struct page *loop_page, unsigned loop_off,
		int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page) + raw_off;
	char *loop_buf = kmap_atomic(loop_page) + loop_off;
	char *in, *out, *key;
	int i, keysize;

	if (cmd == READ) {
		in = raw_buf;
		out = loop_buf;
	} else {
		in = loop_buf;
		out = raw_buf;
	}

	key = lo->lo_encrypt_key;
	keysize = lo->lo_encrypt_key_size;
	for (i = 0; i < size; i++)
		*out++ = *in++ ^ key[(i & 511) % keysize];

	kunmap_atomic(loop_buf);
	kunmap_atomic(raw_buf);
	cond_resched();
	return 0;
}

static int xor_init(struct loop_device *lo, const struct loop_info64 *info)
{
	if (unlikely(info->lo_encrypt_key_size <= 0))
		return -EINVAL;
	return 0;
}

static struct loop_func_table none_funcs = {
	.number = LO_CRYPT_NONE,
	.transfer = transfer_none,
}; 	

static struct loop_func_table xor_funcs = {
	.number = LO_CRYPT_XOR,
	.transfer = transfer_xor,
	.init = xor_init
}; 	

/* xfer_funcs[0] is special - its release function is never called */
static struct loop_func_table *xfer_funcs[MAX_LO_CRYPT] = {
	&none_funcs,
	&xor_funcs
};

static loff_t get_size(loff_t offset, loff_t sizelimit, struct file *file)
{
	loff_t size, loopsize;

	/* Compute loopsize in bytes */
	size = i_size_read(file->f_mapping->host);
	loopsize = size - offset;
	/* offset is beyond i_size, wierd but possible */
	if (loopsize < 0)
		return 0;

	if (sizelimit > 0 && sizelimit < loopsize)
		loopsize = sizelimit;
	/*
	 * Unfortunately, if we want to do I/O on the device,
	 * the number of 512-byte sectors has to fit into a sector_t.
	 */
	return loopsize >> 9;
}

static loff_t get_loop_size(struct loop_device *lo, struct file *file)
{
	return get_size(lo->lo_offset, lo->lo_sizelimit, file);
}

	static int
figure_loop_size(struct loop_device *lo, loff_t offset, loff_t sizelimit)
{
	loff_t size = get_size(offset, sizelimit, lo->lo_backing_file);
	sector_t x = (sector_t)size;

	if (unlikely((loff_t)x != size))
		return -EFBIG;
	if (lo->lo_offset != offset)
		lo->lo_offset = offset;
	if (lo->lo_sizelimit != sizelimit)
		lo->lo_sizelimit = sizelimit;
	set_capacity(lo->lo_disk, x);
	return 0;
}

	static inline int
lo_do_transfer(struct loop_device *lo, int cmd,
		struct page *rpage, unsigned roffs,
		struct page *lpage, unsigned loffs,
		int size, sector_t rblock)
{
	if (unlikely(!lo->transfer))
		return 0;

	return lo->transfer(lo, cmd, rpage, roffs, lpage, loffs, size, rblock);
}

/**
 * __do_lo_send_write - helper for writing data to a loop device
 *
 * This helper just factors out common code between do_lo_send_direct_write()
 * and do_lo_send_write().
 */
static int __do_lo_send_write(struct file *file,
		u8 *buf, const int len, loff_t pos)
{
	ssize_t bw;
	mm_segment_t old_fs = get_fs();

	set_fs(get_ds());
	bw = file->f_op->write(file, buf, len, &pos);
	set_fs(old_fs);
	if (likely(bw == len))
		return 0;
	printk(KERN_ERR "loop: Write error at byte offset %llu, length %i.\n",
			(unsigned long long)pos, len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

/**
 * do_lo_send_direct_write - helper for writing data to a loop device
 *
 * This is the fast, non-transforming version that does not need double
 * buffering.
 */
static int do_lo_send_direct_write(struct loop_device *lo,
		struct bio_vec *bvec, loff_t pos, struct page *page)
{
	ssize_t bw = __do_lo_send_write(lo->lo_backing_file,
			kmap(bvec->bv_page) + bvec->bv_offset,
			bvec->bv_len, pos);
	kunmap(bvec->bv_page);
	cond_resched();
	return bw;
}

/**
 * do_lo_send_write - helper for writing data to a loop device
 *
 * This is the slow, transforming version that needs to double buffer the
 * data as it cannot do the transformations in place without having direct
 * access to the destination pages of the backing file.
 */
static int do_lo_send_write(struct loop_device *lo, struct bio_vec *bvec,
		loff_t pos, struct page *page)
{
	int ret = lo_do_transfer(lo, WRITE, page, 0, bvec->bv_page,
			bvec->bv_offset, bvec->bv_len, pos >> 9);
	if (likely(!ret))
		return __do_lo_send_write(lo->lo_backing_file,
				page_address(page), bvec->bv_len,
				pos);
	printk(KERN_ERR "loop: Transfer error at byte offset %llu, "
			"length %i.\n", (unsigned long long)pos, bvec->bv_len);
	if (ret > 0)
		ret = -EIO;
	return ret;
}

static int lo_send(struct loop_device *lo, struct bio *bio, loff_t pos)
{
	int (*do_lo_send)(struct loop_device *, struct bio_vec *, loff_t,
			struct page *page);
	struct bio_vec *bvec;
	struct page *page = NULL;
	int i, ret = 0;

	if (lo->transfer != transfer_none) {
		page = alloc_page(GFP_NOIO | __GFP_HIGHMEM);
		if (unlikely(!page))
			goto fail;
		kmap(page);
		do_lo_send = do_lo_send_write;
	} else {
		do_lo_send = do_lo_send_direct_write;
	}

	bio_for_each_segment(bvec, bio, i) {
		ret = do_lo_send(lo, bvec, pos, page);
		if (ret < 0)
			break;
		pos += bvec->bv_len;
	}
	if (page) {
		kunmap(page);
		__free_page(page);
	}
out:
	return ret;
fail:
	printk(KERN_ERR "loop: Failed to allocate temporary page for write.\n");
	ret = -ENOMEM;
	goto out;
}

struct lo_read_data {
	struct loop_device *lo;
	struct page *page;
	unsigned offset;
	int bsize;
};

	static int
lo_splice_actor(struct pipe_inode_info *pipe, struct pipe_buffer *buf,
		struct splice_desc *sd)
{
	struct lo_read_data *p = sd->u.data;
	struct loop_device *lo = p->lo;
	struct page *page = buf->page;
	sector_t IV;
	int size;

	IV = ((sector_t) page->index << (PAGE_CACHE_SHIFT - 9)) +
		(buf->offset >> 9);
	size = sd->len;
	if (size > p->bsize)
		size = p->bsize;

	if (lo_do_transfer(lo, READ, page, buf->offset, p->page, p->offset, size, IV)) {
		printk(KERN_ERR "loop: transfer error block %ld\n",
				page->index);
		size = -EINVAL;
	}

	flush_dcache_page(p->page);

	if (size > 0)
		p->offset += size;

	return size;
}

	static int
lo_direct_splice_actor(struct pipe_inode_info *pipe, struct splice_desc *sd)
{
	return __splice_from_pipe(pipe, sd, lo_splice_actor);
}

	static ssize_t
do_lo_receive(struct loop_device *lo,
		struct bio_vec *bvec, int bsize, loff_t pos)
{
	struct lo_read_data cookie;
	struct splice_desc sd;
	struct file *file;
	ssize_t retval;

	cookie.lo = lo;
	cookie.page = bvec->bv_page;
	cookie.offset = bvec->bv_offset;
	cookie.bsize = bsize;

	sd.len = 0;
	sd.total_len = bvec->bv_len;
	sd.flags = 0;
	sd.pos = pos;
	sd.u.data = &cookie;

	file = lo->lo_backing_file;
	retval = splice_direct_to_actor(file, &sd, lo_direct_splice_actor);

	return retval;
}

#define SECTORS_PER_BLOCK	8
#define LO_BLOCK_SIZE		4096
#define BYTES_PER_SECTOR	512
//#define PRE_FETCH_BLOCKS	128 / 4
static int prefetch_blocks=512;
static int bound=4;
module_param(prefetch_blocks,int,0);
module_param(bound,int,0);


struct lo_bio_head {
	struct bio *orig_bio;

	int nr_total;
	int nr_done;
	int error;
	spinlock_t head_lock;

	struct list_head head;
};
struct lo_bio {
	struct list_head slice;

	struct bio *bio;
	struct lo_bio_head *head;

	unsigned int size;
};

void lo_free_bio (struct lo_bio_head *head) {
	struct lo_bio* lo_bio, *n;

	if (head -> nr_done != head -> nr_total) {
		//printk (KERN_EMERG "[BUG]nr_done != nr_total");
		return ;

	}

	list_for_each_entry_safe (lo_bio, n, &head -> head, slice) {
		list_del (&lo_bio -> slice);
		kfree (lo_bio);
	}

}

static void lo_end_bio (struct bio *bio, int error) {
	struct lo_bio *lo_bio= (struct lo_bio *)bio -> bi_private;
	struct lo_bio_head *head;
	struct bio *orig_bio;

	if (error)
		clear_bit (BIO_UPTODATE, &bio->bi_flags);
	else if (!test_bit (BIO_UPTODATE, &bio->bi_flags))
		error = -EIO;

	head = lo_bio -> head;
	spin_lock_irq(&head->head_lock);
	head -> nr_done++;
	spin_unlock_irq(&head->head_lock);
	orig_bio = head -> orig_bio;
	if (!error) {
		orig_bio -> bi_size -= lo_bio -> size;
		orig_bio -> bi_sector += lo_bio -> size >> 9;
	}
	//take care of the bio_pool_idx thing;
	orig_bio -> bi_flags |= (4 << bio -> bi_flags) >> 4;

	if (!head -> error)
		head -> error = error;	

	//printk (KERN_INFO "[DONE]error: %d\n", error);
	bio_put (bio);

	if (head -> nr_total == head -> nr_done) {
		lo_free_bio (head);
		//printk (KERN_INFO "[LO_END_BIO]error: %d\n", head -> error);

		bio_endio(head -> orig_bio, head -> error);
		kfree (head);	
	}	
}

extern void submit_bio (int rw, struct bio *bio);
static void lo_send_bio (struct lo_bio_head *head, struct bio *bio) {
	struct lo_bio *lo_bio = (struct lo_bio *)kmalloc (sizeof (struct lo_bio), GFP_KERNEL); 

	bio -> bi_end_io = lo_end_bio;

	lo_bio -> head = head;
	lo_bio -> size = bio -> bi_size;
	list_add_tail (&lo_bio -> slice, &head -> head);
	bio -> bi_private = (void *)lo_bio;
	lo_bio -> bio = bio;
	head -> nr_total++;
}


sector_t block_cache_lookup (struct loop_device *lo, sector_t blk_local) {
	struct block_cache_node *node;
	//printk (KERN_INFO "[READ_LOCK] read lock obtained \n");
	node = find_cache_node(blk_local,lo);
	//printk (KERN_INFO "[READ_LOCK] read lock released \n");
	if(node!=NULL)
	{
		sector_t blk_global = node -> b_blocknr;
	//	printk (KERN_INFO "[LOOP] block_cache_lookup found block_nr = %lu for blk_local=%lu \n",node->b_blocknr,node->blk_local);
		return blk_global;
	}
	else
	{
	//	printk (KERN_INFO "[LOOP] block uncached for blk_local = %lu \n",blk_local);
		return 0;
	}

}
//performace gain was based on the consumption that:
//this routing was not frequently invoked;
void cio_make_request (struct loop_device *lo,sector_t blk_local,int flag,struct cio_req *cio) {
	//struct cio_req cio;
	cio->create = flag;
	cio->error=0;
	cio->blk_num = prefetch_blocks;
	cio->blk_local=blk_local;
	//cio->req_num=req_num;		
	if(cio->blk_local>100000000)
		printk (KERN_EMERG "[CIO_MAKE_REQUEST] cio->blk_local=%lu\n",cio->blk_local);	
	init_completion (&cio->allocated);
	INIT_LIST_HEAD (&cio->list);

	spin_lock_irq(&lo->lo_cio_lock);
	list_add_tail (&cio->list, &lo->lo_cio_list);
	spin_unlock_irq(&lo->lo_cio_lock);
//	printk (KERN_INFO "[CIO_MAKE_REQUEST] create=%d blk_num=%d blk_local=%lu req_num=%d\n",cio.create,cio.blk_num,cio.blk_local,cio.req_num);
	wake_up(&lo->lo_event);
	wait_for_completion (&cio->allocated);
	if(cio->error)
	{
		printk (KERN_EMERG "[LOOP] wrong cio->error=%d\n",cio->error);
//		cio_make_request(lo,blk_local,flag,cio);
		return;
	}
	return;
}


void cio_make_request_flush (struct loop_device *lo,int flag,struct cio_req *cio) {
	//struct cio_req cio;
	cio->create = flag;
	cio->error=0;
	cio->blk_num = -1;
	
	//cio->req_num=req_num;		
//	if(cio->blk_local>100000000)
//		printk (KERN_EMERG "[CIO_MAKE_REQUEST] cio->blk_local=%lu\n",cio->blk_local);	
	init_completion (&cio->allocated);
	INIT_LIST_HEAD (&cio->list);

	spin_lock_irq(&lo->lo_cio_lock);
	list_add_tail (&cio->list, &lo->lo_cio_list);
	spin_unlock_irq(&lo->lo_cio_lock);
//	printk (KERN_INFO "[CIO_MAKE_REQUEST] create=%d blk_num=%d blk_local=%lu req_num=%d\n",cio.create,cio.blk_num,cio.blk_local,cio.req_num);
	wake_up(&lo->lo_event);
	wait_for_completion (&cio->allocated);
	if(cio->error)
	{
		printk (KERN_EMERG "[LOOP] wrong cio->error=%d\n",cio->error);
//		cio_make_request(lo,blk_local,flag,cio);
		return;
	}
	return;
}

static inline void loop_handle_bio(struct loop_device *, struct bio *);
static void sync_end_bio (struct bio *bio, int error) {
	struct bio *orig_bio= (struct bio *)bio -> bi_private;

	bio_endio(orig_bio,error);
	

}

static int redirect_bio (struct loop_device *lo, struct bio *old_bio) {

	int i;
	struct page *page = NULL;
	struct file *file = lo->lo_backing_file;
	struct inode *inode = file->f_mapping->host;
	struct block_device* bd = inode->i_sb->s_bdev;
	struct lo_bio_head *head;
	struct lo_bio *lo_bio, *n;
	struct bio_vec *bvec;
	const unsigned blkbits = inode->i_blkbits;
	const unsigned sectors_per_block = 1<<(blkbits-9);
	
	sector_t sec_nr_logical,sec_nr_phys;
	sector_t next_sector;
	sector_t blk_logical,blk_phys;
	int cur_page_offset, cur_page_len;
	size_t to_written;
	long nr_total_sectors,remaining_sectors;

	int count, ret;
	struct bio *new_bio = NULL;
	int old_bio_rw;

	loop_handle_bio(lo, old_bio);
	
	head = (struct lo_bio_head *)kmalloc (sizeof (struct lo_bio_head), GFP_KERNEL);
	head -> orig_bio = old_bio;
	head -> nr_total = 0;
	head -> nr_done = 0;
	head -> error = 0;
	spin_lock_init (&head -> head_lock);
	INIT_LIST_HEAD (&head -> head);
	
	//printk(KERN_INFO "[REDIRECT_BIO] bio->bi_sector=%lu\n",bio->bi_sector);

	nr_total_sectors = 0;//bio_sectors(old_bio);
	sec_nr_logical = old_bio -> bi_sector;

	count = 0;
	cur_page_len = 0;
	cur_page_offset = 0;
	next_sector=0;
	bio_for_each_segment (bvec, old_bio, i) {
		nr_total_sectors+=(bvec->bv_len>>9);
	}
	i=old_bio->bi_idx;
	if(nr_total_sectors > i_size_read(inode)){
		bio_endio(old_bio,0);
		return 0;
	}
	
	if((nr_total_sectors == 0)){
		
		if(((old_bio->bi_rw) & (REQ_FLUSH | REQ_FUA))){
			struct cio_req cio;
			
			cio.error = 0;
			if(test_bit(LOOP_DIRTY, &(lo->lo_dirty_state))){
				cio_make_request_flush(lo, old_bio->bi_rw, &cio);
				//bio_endio(old_bio, cio.error);
		//		printk(KERN_INFO "EMPTY FLUSH REQUEST\n");
				//return 0;
			}	
		//	bio_endio(old_bio, cio.error);
			new_bio = bio_alloc(GFP_NOIO, 0);
			new_bio->bi_sector = 0;
			new_bio->bi_bdev = bd;
			new_bio->bi_rw = old_bio->bi_rw;
			new_bio->bi_private = old_bio;
			new_bio->bi_end_io = sync_end_bio;
			
			submit_bio(old_bio->bi_rw, new_bio);
			return 0;

		}
		

	}

	if((old_bio->bi_rw) & REQ_FLUSH){
		struct cio_req cio;
					
		cio.error = 0;
		if(test_bit(LOOP_DIRTY, &(lo->lo_dirty_state))){

		//		printk(KERN_INFO "FLUSH REQUEST\n");
				cio_make_request_flush(lo, old_bio->bi_rw, &cio);
				
		}else {
//		printk(KERN_INFO "FLUSH REQUEST CLEAN");
		}

	}
	
	
	while(nr_total_sectors > 0){
			
		blk_logical = sec_nr_logical >> (blkbits - 9);
		blk_phys = block_cache_lookup(lo, blk_logical);	
		if(!blk_phys)
		{
		//	printk (KERN_INFO "[REDIRECT_BIO] error,blk_global=0 blk_local=%lu\n",blk_local);
			
			struct cio_req cio;

			cio_make_request(lo,blk_logical,old_bio->bi_rw,&cio);
			blk_phys = cio.blk_phy;
		}
	
		sec_nr_phys = (blk_phys << (blkbits - 9)) + (sec_nr_logical % sectors_per_block);
		remaining_sectors = sectors_per_block - (sec_nr_logical % sectors_per_block);
		//printk(KERN_INFO "sec_nr_logical=%lu sec_nr_phys=%lu blk_logical=%lu blk_phys=%lu \n",sec_nr_logical,sec_nr_phys,blk_logical,blk_phys);	
		if(count){
			/*contiguous? */
			if(sec_nr_phys != next_sector){
				lo_send_bio(head, new_bio);
				new_bio = NULL;
			}

		}
		
		if(new_bio == NULL){

			new_bio = bio_alloc(GFP_NOIO, old_bio->bi_vcnt);
			new_bio->bi_sector = sec_nr_phys;
			new_bio->bi_bdev = bd;
			new_bio->bi_rw = old_bio->bi_rw;
		}

		count++;

		if(nr_total_sectors > remaining_sectors)
			nr_total_sectors -= remaining_sectors;
		else{

			remaining_sectors=nr_total_sectors;
			nr_total_sectors=0;
		}
		next_sector = sec_nr_phys + remaining_sectors;
		sec_nr_logical += remaining_sectors;	
		while(remaining_sectors > 0){

			//printk(KERN_INFO "cur page len %ld \n", cur_page_len);
	
			if(cur_page_len == 0){

				page = old_bio->bi_io_vec[i].bv_page;
			//	printk(KERN_INFO "i %d\n", i);
				cur_page_offset = old_bio->bi_io_vec[i].bv_offset;
				cur_page_len = old_bio->bi_io_vec[i].bv_len;
				if(cur_page_len%512!=0)
					printk(KERN_EMERG "cur_page_len error cur_page_len=%d start_idx=%d i=%d bi_vcnt=%d nr_total_sectors=%lu sec_nr_logical=%lu sec_nr_phys=%lu remaining_sectors=%d \n",cur_page_len,old_bio->bi_idx,i,old_bio->bi_vcnt,nr_total_sectors,sec_nr_logical,sec_nr_phys,remaining_sectors);	
			//	printk(KERN_INFO "cur page offset %lu len %lu\n", cur_page_offset, cur_page_len);
				i++;

			}	
			
			to_written = remaining_sectors > (cur_page_len >> 9)? (cur_page_len >> 9):remaining_sectors;
			
			if(new_bio == NULL)
				printk(KERN_INFO "bio error\n");
			if(page == NULL)
				printk(KERN_INFO "page error\n");
			//printk(KERN_INFO "new_bio->bi_vcnt %d offset %ld to written %ld\n", new_bio->bi_vcnt, cur_page_offset, to_written);
			ret = bio_add_page(new_bio, page, (to_written << 9), cur_page_offset);
			
			remaining_sectors -= to_written;
			cur_page_offset += (to_written << 9);
			cur_page_len -= (to_written << 9);
			//printk(KERN_INFO "add page len %ld remaining_sectors=%lu\n", ret,remaining_sectors);
			
			
		}
			

	}

	old_bio_rw = old_bio->bi_rw;

	if(count)
		lo_send_bio (head, new_bio);

	if (count) {
		//printk (KERN_INFO "[REDIRECT_BIO] new_bio: offset=%d count=%d start=%lu i=%d\n",offset,count,start_sect,i);
		/*
		 *this is syncrounous for RAMDISK!
		 *and the lo_end_bio will be called through the path
		 */
		/*This old way may cause list failure at the last step of the MACRO(check the head for termination)
		  list_for_each_entry_safe(lo_bio, n, &head -> head, slice) 
		  submit_bio (bio -> bi_rw, lo_bio -> bio);
		 */

		//We need the new way:terminate at the last lo_bio(not at the head),
		//Thus lo_end_bio will not cause list failure;
		struct list_head *l_head = &head -> head;
		struct lo_bio* last = list_entry (l_head -> prev, typeof (*lo_bio), slice);
	
		for (lo_bio = list_entry (l_head -> next, typeof (*lo_bio), slice);lo_bio != last; 
				lo_bio = list_entry(lo_bio->slice.next, typeof (*lo_bio), slice))
			submit_bio (old_bio -> bi_rw, lo_bio -> bio);
		submit_bio (old_bio -> bi_rw, last -> bio);
	}
	else //can not find a block, we just end it.
	{
		bio_endio (old_bio, 0);
		//printk (KERN_INFO "[REDIRECT_BIO] the io of bio is ended with count 0");
	}


	if((old_bio_rw) & REQ_FUA){
		struct cio_req cio;
					
		cio.error = 0;
		if(test_bit(LOOP_DIRTY, &(lo->lo_dirty_state))){
				cio_make_request_flush(lo, old_bio_rw, &cio);
		//		printk(KERN_INFO "FUA REQUEST\n");
			}else{
		//		printk(KERN_INFO "FUA REQUEST CLEAN\n");
			}
	}
	
	return 0;

}

	static int
lo_receive(struct loop_device *lo, struct bio *bio, int bsize, loff_t pos)
{
	int i;
	ssize_t s;
	struct bio_vec *bvec;

	bio_for_each_segment(bvec, bio, i) {
		s = do_lo_receive(lo, bvec, bsize, pos);
		if (s < 0)
			return s;

		if (s != bvec->bv_len) {
			zero_fill_bio(bio);
			break;
		}
		pos += bvec->bv_len;
	}

	return 0;
}

static int do_bio_filebacked(struct loop_device *lo, struct bio *bio)
{
	loff_t pos;
	int ret;

	pos = ((loff_t) bio->bi_sector << 9) + lo->lo_offset;

	if (bio_rw(bio) == WRITE) {
		struct file *file = lo->lo_backing_file;

		if (bio->bi_rw & REQ_FLUSH) {
			ret = vfs_fsync(file, 0);
			if (unlikely(ret && ret != -EINVAL)) {
				ret = -EIO;
				goto out;
			}
		}

		/*
		 * We use punch hole to reclaim the free space used by the
		 * image a.k.a. discard. However we do not support discard if
		 * encryption is enabled, because it may give an attacker
		 * useful information.
		 */
		if (bio->bi_rw & REQ_DISCARD) {
			struct file *file = lo->lo_backing_file;
			int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

			if ((!file->f_op->fallocate) ||
					lo->lo_encrypt_key_size) {
				ret = -EOPNOTSUPP;
				goto out;
			}
			ret = file->f_op->fallocate(file, mode, pos,
					bio->bi_size);
			if (unlikely(ret && ret != -EINVAL &&
						ret != -EOPNOTSUPP))
				ret = -EIO;
			goto out;
		}

		ret = lo_send(lo, bio, pos);

		if ((bio->bi_rw & REQ_FUA) && !ret) {
			ret = vfs_fsync(file, 0);
			if (unlikely(ret && ret != -EINVAL))
				ret = -EIO;
		}
	} else
	{ 
		//ret = lo_receive(lo, bio, lo->lo_blocksize, pos);
		//printk (KERN_INFO "[LOOP] make redirect_bio request from do_bio_filebacked\n");
		ret = redirect_bio (lo, bio);
	}
out:
	return ret;
}

/*
 * Add bio to back of pending list
 */
static void loop_add_bio(struct loop_device *lo, struct bio *bio)
{
	lo->lo_bio_count++;
	bio_list_add(&lo->lo_bio_list, bio);

	//printk (KERN_INFO "[LOOP_REQS]%d\n", lo-> lo_bio_count);
}

/*
 * Grab first pending buffer
 */
static struct bio *loop_get_bio(struct loop_device *lo)
{
	struct bio* bio = bio_list_pop(&lo->lo_bio_list);
	if (bio)
		lo->lo_bio_count--;

	return bio;
}
static void loop_make_request(struct request_queue *q, struct bio *old_bio)
{
	struct loop_device *lo = q->queuedata;
	int rw = bio_rw(old_bio);

	if (rw == READA)
		rw = READ;

	BUG_ON(!lo || (rw != READ && rw != WRITE));



	struct bio_list* current_list = current -> bio_list;
	current -> bio_list = NULL;
	//printk (KERN_INFO "[LOOP] make redirect_bio request from loop_make_request() \n");
	redirect_bio (lo, old_bio);
	current -> bio_list = current_list;

	return;
}

struct switch_request {
	struct file *file;
	struct completion wait;
};

static void do_loop_switch(struct loop_device *, struct switch_request *);

static inline void loop_handle_bio(struct loop_device *lo, struct bio *bio)
{
	if (unlikely(!bio->bi_bdev)) {
		do_loop_switch(lo, bio->bi_private);
		bio_put(bio);
	} /*else {
		
		   int ret = do_bio_filebacked(lo, bio);
		   if (bio_rw(bio) == WRITE)
		   bio_endio(bio, ret);
		 



		//which can come to happen in Direct-I/O
		if (bio -> bi_size == 0)
			bio_endio (bio, 0);
		else 
			redirect_bio (lo, bio);
	}*/
}

static void cpu_bound(int num )
{
	int i;
	struct cpumask allowed_mask;

	if(num == 0){
		printk(KERN_INFO "loop bound 0\n");
		return;
	}

	memset (&allowed_mask, 0, sizeof (struct cpumask));
	for(i=0;i<num;i++)
	{
		cpumask_set_cpu (i, &allowed_mask);
	}
	set_cpus_allowed_ptr (current, &allowed_mask);

	printk(KERN_INFO "loop bound %d\n", num);

	return;
}

extern int set_cpus_allowed_ptr (struct task_struct *p, const struct cpumask *new_mask);
/*
 * worker thread that handles reads/writes to file backed loop devices,
 * to avoid blocking in our make_request_fn. it also does loop decrypting
 * on reads for block backed loop, as that is too heavy to do from
 * b_end_io context where irqs may be disabled.
 *
 * Loop explanation:  loop_clr_fd() sets lo_state to Lo_rundown before
 * calling kthread_stop().  Therefore once kthread_should_stop() is
 * true, make_request will not place any more requests.  Therefore
 * once kthread_should_stop() is true and lo_bio is NULL, we are
 * done with the loop.
 */

static int sync_file(struct loop_device *lo, struct file *file, int create)
{
	int ret = 0;

	if((create & (REQ_FUA | REQ_FLUSH))){
		
		ret = vfs_fsync(file, 0);
		if(!ret)
			clear_bit(LOOP_DIRTY, &(lo->lo_dirty_state));
		else 
			printk(KERN_ERR "Multilanes fsync error %d\n", ret);
		
	}

	return ret;

}


static int loop_thread(void *data)
{
	struct loop_device *lo = data;
	struct bio *bio;
	//struct cpumask allowed_mask;
	struct buffer_head bh_result;
	struct file *file = lo -> lo_backing_file;
	struct inode *inode = file -> f_mapping -> host;

	

	cpu_bound(bound);
	

	set_user_nice(current, -20);

	while (!kthread_should_stop() || !list_empty(&lo->lo_cio_list)) {
		
		int ret = wait_event_interruptible_exclusive(lo->lo_event,
				!list_empty (&lo->lo_cio_list) || kthread_should_stop ());
		

		if (list_empty (&lo->lo_cio_list))
		{	
//			printk (KERN_INFO "[LOOP] lo->lo_cio_list is empty\n");
			continue;
		}
		
		spin_lock_irq(&lo->lo_cio_lock);
		struct list_head *present = lo -> lo_cio_list.next;
		struct cio_req *cio = list_entry (present, struct cio_req, list);
		list_del_init (present);
		spin_unlock_irq(&lo->lo_cio_lock);
	
/*
		if (lo -> block_table[cio -> blk_local]) {
			printk (KERN_INFO "[CACHE_FETCH]req already completed.blk: %d\n", cio -> blk_local);
			complete (&cio -> allocated);
			continue;
		}
*/

		
		if(cio->blk_num == -1){
			
			ret = vfs_fsync(file, 0);
			
			cio->error = ret;
			if(!ret){
				clear_bit(LOOP_DIRTY, &(lo->lo_dirty_state));
	//			printk(KERN_INFO "clear bit");
			}
			 else{
			 	printk(KERN_ERR "Multilanes flush err %d\n", ret);
			 }
		
	//	 	printk(KERN_INFO "fsync finished\n");	 
			complete(&cio->allocated);
			
			continue;


		}
		
		int i = 0;
		int req_num=cio->req_num;
		sector_t blk_start = cio -> blk_local;
		int create=(cio->create) & WRITE;
		//printk (KERN_INFO "[CACHE_FETCH]new request received: %d, empty: %d\n", cio -> blk_num, list_empty (&lo->lo_cio_list));
		struct block_cache_node *cur=find_cache_node(blk_start,lo);


		

		while (i < prefetch_blocks) {
			//if (!lo -> block_table[blk_start]) {
			if(cur== NULL){
				memset (&bh_result, 0, sizeof (struct buffer_head));
				bh_result.b_state = 0;
				bh_result.b_blocknr =0;
				//it's been proven that this arg is extremely IMPORTANT!
				//bh_result.b_size= LO_BLOCK_SIZE;
				if(inode==NULL)
					printk(KERN_EMERG "[LOOP] inode ==null\n");
				bh_result.b_size = (prefetch_blocks-i)<<inode->i_blkbits;
				//printk (KERN_INFO "[LOOP]inode: 0x%x, blk_start: %lu, bh_result: 0x%x create: %d req_num=%d\n", inode, blk_start, &bh_result,create,req_num);
				int ret = inode -> i_op -> get_block (inode, blk_start, &bh_result, create);
	//			if (create){
	//				printk(KERN_INFO "get mapping");
	//			}
				int got_num= bh_result.b_size>>inode->i_blkbits;
				//i +=got_num;
				//cio->slice++;
				if (ret) {
					printk (KERN_EMERG "[ALERT]block not found: %lu cio->req_num=%d i=%d got_num=%d cio->blk_local=%lu ret=%n \n", blk_start,req_num,i,got_num,cio->blk_local,ret);
					cio->error=ret;
					complete(&cio->allocated);
					break;
				}
				else {
					int j = 0;
					
					if(create){
						set_bit(LOOP_DIRTY, &(lo->lo_dirty_state));
			//			printk(KERN_INFO "set dirt\n");
					}
					
					while(j<got_num)
					{
						if(bh_result.b_blocknr)
						{
							//printk (KERN_INFO "[LOOP] add cache: blk_local=%lu b_blocknr=%lu i=%d got_num=%d cio->req_num=%d\n",blk_start+j,bh_result.b_blocknr+j,i,got_num,req_num);
							if((!i)&&(!j))
							{
								cio->error=0;
								cio->blk_phy=bh_result.b_blocknr;
								
								
								//cio->error = sync_file(lo, file, cio->create);
								
								
								complete (&cio->allocated);
								
							}
							add_cache(bh_result.b_blocknr+j, blk_start+j,lo);
						}
						else
						{
							if((!i)&&(!j)){
								cio->error=0;
								cio->blk_phy=0;
								
								//cio->error = sync_file(lo, file, cio->create);
								
								complete (&cio->allocated);
								
							}
							i++;
							blk_start++;
							break;
						}
						j++;
					}
						i += j;
						blk_start+=j;
						//printk(KERN_INFO "[LOOP] i=%d blk_start=%lu\n",i,blk_start);
				}
			}
			else {
				if(!i)
				{
					cio->error=0;
					cio->blk_phy=cur->b_blocknr;
					
					//cio->error = sync_file(lo, file, cio->create);
					
					
					complete(&cio->allocated);
					
					break;
					//printk (KERN_INFO "[LOOP] about to send complete info after read cache cio->blk[%d]= %lu blk_local=%lu\n",i,cio->blk_phy[i],blk_start);
				}
			}
			//printk (KERN_INFO "[CACHE_FETCH]i: %d, num: %d, blk_start: %d, total: %d\n", i, cio -> blk_num, blk_start, BLOCK_COUNT);
//			blk_start++;
//			i++;
		}
		//printk (KERN_INFO "[CACHE_FETCH]%d blocks prefetched.blk_start: %d\n", i, blk_start);


	}

	
	return 0;
}

/*
 * loop_switch performs the hard work of switching a backing store.
 * First it needs to flush existing IO, it does this by sending a magic
 * BIO down the pipe. The completion of this BIO does the actual switch.
 */
static int loop_switch(struct loop_device *lo, struct file *file)
{
	struct switch_request w;
	struct bio *bio = bio_alloc(GFP_KERNEL, 0);
	if (!bio)
		return -ENOMEM;
	init_completion(&w.wait);
	w.file = file;
	bio->bi_private = &w;
	bio->bi_bdev = NULL;
	loop_make_request(lo->lo_queue, bio);
	//printk(KERN_INFO "[LOOP] call loop_make_request reom loop_switch()\n");
	wait_for_completion(&w.wait);
	return 0;
}

/*
 * Helper to flush the IOs in loop, but keeping loop thread running
 */
static int loop_flush(struct loop_device *lo)
{
	/* loop not yet configured, no running thread, nothing to flush */
	if (!lo->lo_thread)
		return 0;
	//printk (KERN_INFO "[LOOP] LOOP is about to flush \n");
	return loop_switch(lo, NULL);
}

/*
 * Do the actual switch; called from the BIO completion routine
 */
static void do_loop_switch(struct loop_device *lo, struct switch_request *p)
{
	struct file *file = p->file;
	struct file *old_file = lo->lo_backing_file;
	struct address_space *mapping;

	/* if no new file, only flush of queued bios requested */
	if (!file)
		goto out;

	mapping = file->f_mapping;
	mapping_set_gfp_mask(old_file->f_mapping, lo->old_gfp_mask);
	lo->lo_backing_file = file;
	lo->lo_blocksize = S_ISBLK(mapping->host->i_mode) ?
		mapping->host->i_bdev->bd_block_size : PAGE_SIZE;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));
out:
	complete(&p->wait);
}


/*
 * loop_change_fd switched the backing store of a loopback device to
 * a new file. This is useful for operating system installers to free up
 * the original file and in High Availability environments to switch to
 * an alternative location for the content in case of server meltdown.
 * This can only work if the loop device is used read-only, and if the
 * new backing store is the same size and type as the old backing store.
 */
static int loop_change_fd(struct loop_device *lo, struct block_device *bdev,
		unsigned int arg)
{
	struct file	*file, *old_file;
	struct inode	*inode;
	int		error;

	error = -ENXIO;
	if (lo->lo_state != Lo_bound)
		goto out;

	/* the loop device has to be read-only */
	error = -EINVAL;
	if (!(lo->lo_flags & LO_FLAGS_READ_ONLY))
		goto out;

	error = -EBADF;
	file = fget(arg);
	if (!file)
		goto out;

	inode = file->f_mapping->host;
	old_file = lo->lo_backing_file;

	error = -EINVAL;

	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		goto out_putf;

	/* size of the new backing store needs to be the same */
	if (get_loop_size(lo, file) != get_loop_size(lo, old_file))
		goto out_putf;

	/* and ... switch */
	error = loop_switch(lo, file);
	if (error)
		goto out_putf;

	fput(old_file);
	if (lo->lo_flags & LO_FLAGS_PARTSCAN)
		ioctl_by_bdev(bdev, BLKRRPART, 0);
	return 0;

out_putf:
	fput(file);
out:
	return error;
}

static inline int is_loop_device(struct file *file)
{
	struct inode *i = file->f_mapping->host;

	return i && S_ISBLK(i->i_mode) && MAJOR(i->i_rdev) == LOOP_MAJOR;
}

/* loop sysfs attributes */

static ssize_t loop_attr_show(struct device *dev, char *page,
		ssize_t (*callback)(struct loop_device *, char *))
{
	struct gendisk *disk = dev_to_disk(dev);
	struct loop_device *lo = disk->private_data;

	return callback(lo, page);
}

#define LOOP_ATTR_RO(_name)						\
	static ssize_t loop_attr_##_name##_show(struct loop_device *, char *);	\
static ssize_t loop_attr_do_show_##_name(struct device *d,		\
		struct device_attribute *attr, char *b)	\
{									\
	return loop_attr_show(d, b, loop_attr_##_name##_show);		\
}									\
static struct device_attribute loop_attr_##_name =			\
__ATTR(_name, S_IRUGO, loop_attr_do_show_##_name, NULL);

static ssize_t loop_attr_backing_file_show(struct loop_device *lo, char *buf)
{
	ssize_t ret;
	char *p = NULL;

	spin_lock_irq(&lo->lo_lock);
	if (lo->lo_backing_file)
		p = d_path(&lo->lo_backing_file->f_path, buf, PAGE_SIZE - 1);
	spin_unlock_irq(&lo->lo_lock);

	if (IS_ERR_OR_NULL(p))
		ret = PTR_ERR(p);
	else {
		ret = strlen(p);
		memmove(buf, p, ret);
		buf[ret++] = '\n';
		buf[ret] = 0;
	}

	return ret;
}

static ssize_t loop_attr_offset_show(struct loop_device *lo, char *buf)
{
	return sprintf(buf, "%llu\n", (unsigned long long)lo->lo_offset);
}

static ssize_t loop_attr_sizelimit_show(struct loop_device *lo, char *buf)
{
	return sprintf(buf, "%llu\n", (unsigned long long)lo->lo_sizelimit);
}

static ssize_t loop_attr_autoclear_show(struct loop_device *lo, char *buf)
{
	int autoclear = (lo->lo_flags & LO_FLAGS_AUTOCLEAR);

	return sprintf(buf, "%s\n", autoclear ? "1" : "0");
}

static ssize_t loop_attr_partscan_show(struct loop_device *lo, char *buf)
{
	int partscan = (lo->lo_flags & LO_FLAGS_PARTSCAN);

	return sprintf(buf, "%s\n", partscan ? "1" : "0");
}

LOOP_ATTR_RO(backing_file);
LOOP_ATTR_RO(offset);
LOOP_ATTR_RO(sizelimit);
LOOP_ATTR_RO(autoclear);
LOOP_ATTR_RO(partscan);

static struct attribute *loop_attrs[] = {
	&loop_attr_backing_file.attr,
	&loop_attr_offset.attr,
	&loop_attr_sizelimit.attr,
	&loop_attr_autoclear.attr,
	&loop_attr_partscan.attr,
	NULL,
};

static struct attribute_group loop_attribute_group = {
	.name = "loop",
	.attrs= loop_attrs,
};

static int loop_sysfs_init(struct loop_device *lo)
{
	return sysfs_create_group(&disk_to_dev(lo->lo_disk)->kobj,
			&loop_attribute_group);
}

static void loop_sysfs_exit(struct loop_device *lo)
{
	sysfs_remove_group(&disk_to_dev(lo->lo_disk)->kobj,
			&loop_attribute_group);
}

static void loop_config_discard(struct loop_device *lo)
{
	struct file *file = lo->lo_backing_file;
	struct inode *inode = file->f_mapping->host;
	struct request_queue *q = lo->lo_queue;

	/*
	 * We use punch hole to reclaim the free space used by the
	 * image a.k.a. discard. However we do support discard if
	 * encryption is enabled, because it may give an attacker
	 * useful information.
	 */
	if ((!file->f_op->fallocate) ||
			lo->lo_encrypt_key_size) {
		q->limits.discard_granularity = 0;
		q->limits.discard_alignment = 0;
		q->limits.max_discard_sectors = 0;
		q->limits.discard_zeroes_data = 0;
		queue_flag_clear_unlocked(QUEUE_FLAG_DISCARD, q);
		return;
	}

	q->limits.discard_granularity = inode->i_sb->s_blocksize;
	q->limits.discard_alignment = 0;
	q->limits.max_discard_sectors = UINT_MAX >> 9;
	q->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
}

static int loop_set_fd(struct loop_device *lo, fmode_t mode,
		struct block_device *bdev, unsigned int arg)
{
	struct file	*file, *f;
	struct inode	*inode;
	struct address_space *mapping;
	unsigned lo_blocksize;
	int		lo_flags = 0;
	int		error;
	loff_t		size;

	/* This is safe, since we have a reference from open(). */
	__module_get(THIS_MODULE);

	error = -EBADF;
	file = fget(arg);
	if (!file)
		goto out;

	error = -EBUSY;
	if (lo->lo_state != Lo_unbound)
		goto out_putf;

	/* Avoid recursion */
	f = file;
	while (is_loop_device(f)) {
		struct loop_device *l;

		if (f->f_mapping->host->i_bdev == bdev)
			goto out_putf;

		l = f->f_mapping->host->i_bdev->bd_disk->private_data;
		if (l->lo_state == Lo_unbound) {
			error = -EINVAL;
			goto out_putf;
		}
		f = l->lo_backing_file;
	}

	mapping = file->f_mapping;
	inode = mapping->host;

	error = -EINVAL;
	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		goto out_putf;

	if (!(file->f_mode & FMODE_WRITE) || !(mode & FMODE_WRITE) ||
			!file->f_op->write)
		lo_flags |= LO_FLAGS_READ_ONLY;

	lo_blocksize = S_ISBLK(inode->i_mode) ?
		inode->i_bdev->bd_block_size : PAGE_SIZE;

	error = -EFBIG;
	size = get_loop_size(lo, file);
	if ((loff_t)(sector_t)size != size)
		goto out_putf;

	error = 0;

	set_device_ro(bdev, (lo_flags & LO_FLAGS_READ_ONLY) != 0);

	lo->lo_blocksize = lo_blocksize;
	lo->lo_device = bdev;
	lo->lo_flags = lo_flags;
	lo->lo_backing_file = file;
	lo->transfer = transfer_none;
	lo->ioctl = NULL;
	lo->lo_sizelimit = 0;
	lo->lo_bio_count = 0;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));

	bio_list_init(&lo->lo_bio_list);
	lo->lo_dirty_state = 0;

	/*
	 * set queue make_request_fn, and add limits based on lower level
	 * device
	 */
	blk_queue_make_request(lo->lo_queue, loop_make_request);
	lo->lo_queue->queuedata = lo;

	if (!(lo_flags & LO_FLAGS_READ_ONLY) && file->f_op->fsync)
		blk_queue_flush(lo->lo_queue, REQ_FLUSH);

	set_capacity(lo->lo_disk, size);
	bd_set_size(bdev, size << 9);
	loop_sysfs_init(lo);
	/* let user-space know about the new size */
	kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);

	set_blocksize(bdev, lo_blocksize);

    
	lo->lo_thread = kthread_create(loop_thread, lo, "loop%d", lo->lo_number);

	
	//lo->lo_thread1 = kthread_create (loop_thread, lo, "loop%d", lo -> lo_number);
	/*	lo->lo_thread2 = kthread_create (loop_thread, lo, "loop%d", lo -> lo_number);
		lo->lo_thread3 = kthread_create (loop_thread, lo, "loop%d", lo -> lo_number);
	 */
/*
	int i = 0;
	while (i < BLOCK_COUNT) {
		lo -> block_table[i] = 0;
		i++;
	}
*/
	atomic_set(&lo -> lru_count ,0);
	spin_lock_init (&lo->table_lock);
	INIT_LIST_HEAD (&lo -> lru);
	INIT_LIST_HEAD (&lo -> lo_cio_list);
	spin_lock_init (&lo -> lo_cio_lock);
	spin_lock_init (&lo -> lru_lock);
	int i=0;
	while(i<CACHE_COUNT)
	{
		spin_lock_init (&lo -> cache_lock[i]);
		i++;
	}
	init_cache(lo);
	

	if (IS_ERR(lo->lo_thread)) {
		error = PTR_ERR(lo->lo_thread);
		goto out_clr;
	}
	lo->lo_state = Lo_bound;
	wake_up_process(lo->lo_thread);
	//wake_up_process(lo->lo_thread1);
	/*	wake_up_process(lo->lo_thread2);
		wake_up_process(lo->lo_thread3);
	 */
	if (part_shift)
		lo->lo_flags |= LO_FLAGS_PARTSCAN;
	if (lo->lo_flags & LO_FLAGS_PARTSCAN)
		ioctl_by_bdev(bdev, BLKRRPART, 0);
	return 0;

out_clr:
	loop_sysfs_exit(lo);
	lo->lo_thread = NULL;
	lo->lo_device = NULL;
	lo->lo_backing_file = NULL;
	lo->lo_flags = 0;
	set_capacity(lo->lo_disk, 0);
	invalidate_bdev(bdev);
	bd_set_size(bdev, 0);
	kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask);
	lo->lo_state = Lo_unbound;
out_putf:
	fput(file);
out:
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	return error;
}

	static int
loop_release_xfer(struct loop_device *lo)
{
	int err = 0;
	struct loop_func_table *xfer = lo->lo_encryption;

	if (xfer) {
		if (xfer->release)
			err = xfer->release(lo);
		lo->transfer = NULL;
		lo->lo_encryption = NULL;
		module_put(xfer->owner);
	}
	return err;
}

	static int
loop_init_xfer(struct loop_device *lo, struct loop_func_table *xfer,
		const struct loop_info64 *i)
{
	int err = 0;

	if (xfer) {
		struct module *owner = xfer->owner;

		if (!try_module_get(owner))
			return -EINVAL;
		if (xfer->init)
			err = xfer->init(lo, i);
		if (err)
			module_put(owner);
		else
			lo->lo_encryption = xfer;
	}
	return err;
}

static int loop_clr_fd(struct loop_device *lo)
{
	struct file *filp = lo->lo_backing_file;
	gfp_t gfp = lo->old_gfp_mask;
	struct block_device *bdev = lo->lo_device;

	if (lo->lo_state != Lo_bound)
		return -ENXIO;

	/*
	 * If we've explicitly asked to tear down the loop device,
	 * and it has an elevated reference count, set it for auto-teardown when
	 * the last reference goes away. This stops $!~#$@ udev from
	 * preventing teardown because it decided that it needs to run blkid on
	 * the loopback device whenever they appear. xfstests is notorious for
	 * failing tests because blkid via udev races with a losetup
	 * <dev>/do something like mkfs/losetup -d <dev> causing the losetup -d
	 * command to fail with EBUSY.
	 */
	if (lo->lo_refcnt > 1) {
		lo->lo_flags |= LO_FLAGS_AUTOCLEAR;
		mutex_unlock(&lo->lo_ctl_mutex);
		return 0;
	}

	if (filp == NULL)
		return -EINVAL;

	spin_lock_irq(&lo->lo_lock);
	lo->lo_state = Lo_rundown;
	spin_unlock_irq(&lo->lo_lock);

	kthread_stop(lo->lo_thread);
	//kthread_stop(lo->lo_thread1);
	/*kthread_stop(lo->lo_thread2);
	  kthread_stop(lo->lo_thread3);
	 */
	spin_lock_irq(&lo->lo_lock);
	lo->lo_backing_file = NULL;
	spin_unlock_irq(&lo->lo_lock);

	loop_release_xfer(lo);
	lo->transfer = NULL;
	lo->ioctl = NULL;
	lo->lo_device = NULL;
	lo->lo_encryption = NULL;
	lo->lo_offset = 0;
	lo->lo_sizelimit = 0;
	lo->lo_encrypt_key_size = 0;
	lo->lo_thread = NULL;
	memset(lo->lo_encrypt_key, 0, LO_KEY_SIZE);
	memset(lo->lo_crypt_name, 0, LO_NAME_SIZE);
	memset(lo->lo_file_name, 0, LO_NAME_SIZE);
	if (bdev)
		invalidate_bdev(bdev);
	set_capacity(lo->lo_disk, 0);
	loop_sysfs_exit(lo);
	if (bdev) {
		bd_set_size(bdev, 0);
		/* let user-space know about this change */
		kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	}
	mapping_set_gfp_mask(filp->f_mapping, gfp);
	lo->lo_state = Lo_unbound;
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	if (lo->lo_flags & LO_FLAGS_PARTSCAN && bdev)
		ioctl_by_bdev(bdev, BLKRRPART, 0);
	lo->lo_flags = 0;
	if (!part_shift)
		lo->lo_disk->flags |= GENHD_FL_NO_PART_SCAN;
	mutex_unlock(&lo->lo_ctl_mutex);
	/*
	 * Need not hold lo_ctl_mutex to fput backing file.
	 * Calling fput holding lo_ctl_mutex triggers a circular
	 * lock dependency possibility warning as fput can take
	 * bd_mutex which is usually taken before lo_ctl_mutex.
	 */
	fput(filp);
	return 0;
}

	static int
loop_set_status(struct loop_device *lo, const struct loop_info64 *info)
{
	int err;
	struct loop_func_table *xfer;
	kuid_t uid = current_uid();

	if (lo->lo_encrypt_key_size &&
			!uid_eq(lo->lo_key_owner, uid) &&
			!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (lo->lo_state != Lo_bound)
		return -ENXIO;
	if ((unsigned int) info->lo_encrypt_key_size > LO_KEY_SIZE)
		return -EINVAL;

	err = loop_release_xfer(lo);
	if (err)
		return err;

	if (info->lo_encrypt_type) {
		unsigned int type = info->lo_encrypt_type;

		if (type >= MAX_LO_CRYPT)
			return -EINVAL;
		xfer = xfer_funcs[type];
		if (xfer == NULL)
			return -EINVAL;
	} else
		xfer = NULL;

	err = loop_init_xfer(lo, xfer, info);
	if (err)
		return err;

	if (lo->lo_offset != info->lo_offset ||
			lo->lo_sizelimit != info->lo_sizelimit) {
		if (figure_loop_size(lo, info->lo_offset, info->lo_sizelimit))
			return -EFBIG;
	}
	loop_config_discard(lo);

	memcpy(lo->lo_file_name, info->lo_file_name, LO_NAME_SIZE);
	memcpy(lo->lo_crypt_name, info->lo_crypt_name, LO_NAME_SIZE);
	lo->lo_file_name[LO_NAME_SIZE-1] = 0;
	lo->lo_crypt_name[LO_NAME_SIZE-1] = 0;

	if (!xfer)
		xfer = &none_funcs;
	lo->transfer = xfer->transfer;
	lo->ioctl = xfer->ioctl;

	if ((lo->lo_flags & LO_FLAGS_AUTOCLEAR) !=
			(info->lo_flags & LO_FLAGS_AUTOCLEAR))
		lo->lo_flags ^= LO_FLAGS_AUTOCLEAR;

	if ((info->lo_flags & LO_FLAGS_PARTSCAN) &&
			!(lo->lo_flags & LO_FLAGS_PARTSCAN)) {
		lo->lo_flags |= LO_FLAGS_PARTSCAN;
		lo->lo_disk->flags &= ~GENHD_FL_NO_PART_SCAN;
		ioctl_by_bdev(lo->lo_device, BLKRRPART, 0);
	}

	lo->lo_encrypt_key_size = info->lo_encrypt_key_size;
	lo->lo_init[0] = info->lo_init[0];
	lo->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_key_size) {
		memcpy(lo->lo_encrypt_key, info->lo_encrypt_key,
				info->lo_encrypt_key_size);
		lo->lo_key_owner = uid;
	}	

	return 0;
}

	static int
loop_get_status(struct loop_device *lo, struct loop_info64 *info)
{
	struct file *file = lo->lo_backing_file;
	struct kstat stat;
	int error;

	if (lo->lo_state != Lo_bound)
		return -ENXIO;
	error = vfs_getattr(file->f_path.mnt, file->f_path.dentry, &stat);
	if (error)
		return error;
	memset(info, 0, sizeof(*info));
	info->lo_number = lo->lo_number;
	info->lo_device = huge_encode_dev(stat.dev);
	info->lo_inode = stat.ino;
	info->lo_rdevice = huge_encode_dev(lo->lo_device ? stat.rdev : stat.dev);
	info->lo_offset = lo->lo_offset;
	info->lo_sizelimit = lo->lo_sizelimit;
	info->lo_flags = lo->lo_flags;
	memcpy(info->lo_file_name, lo->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_crypt_name, lo->lo_crypt_name, LO_NAME_SIZE);
	info->lo_encrypt_type =
		lo->lo_encryption ? lo->lo_encryption->number : 0;
	if (lo->lo_encrypt_key_size && capable(CAP_SYS_ADMIN)) {
		info->lo_encrypt_key_size = lo->lo_encrypt_key_size;
		memcpy(info->lo_encrypt_key, lo->lo_encrypt_key,
				lo->lo_encrypt_key_size);
	}
	return 0;
}

	static void
loop_info64_from_old(const struct loop_info *info, struct loop_info64 *info64)
{
	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info->lo_number;
	info64->lo_device = info->lo_device;
	info64->lo_inode = info->lo_inode;
	info64->lo_rdevice = info->lo_rdevice;
	info64->lo_offset = info->lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info->lo_encrypt_type;
	info64->lo_encrypt_key_size = info->lo_encrypt_key_size;
	info64->lo_flags = info->lo_flags;
	info64->lo_init[0] = info->lo_init[0];
	info64->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info->lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info->lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info->lo_encrypt_key, LO_KEY_SIZE);
}

	static int
loop_info64_to_old(const struct loop_info64 *info64, struct loop_info *info)
{
	memset(info, 0, sizeof(*info));
	info->lo_number = info64->lo_number;
	info->lo_device = info64->lo_device;
	info->lo_inode = info64->lo_inode;
	info->lo_rdevice = info64->lo_rdevice;
	info->lo_offset = info64->lo_offset;
	info->lo_encrypt_type = info64->lo_encrypt_type;
	info->lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info->lo_flags = info64->lo_flags;
	info->lo_init[0] = info64->lo_init[0];
	info->lo_init[1] = info64->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info->lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info->lo_device != info64->lo_device ||
			info->lo_rdevice != info64->lo_rdevice ||
			info->lo_inode != info64->lo_inode ||
			info->lo_offset != info64->lo_offset)
		return -EOVERFLOW;

	return 0;
}

	static int
loop_set_status_old(struct loop_device *lo, const struct loop_info __user *arg)
{
	struct loop_info info;
	struct loop_info64 info64;

	if (copy_from_user(&info, arg, sizeof (struct loop_info)))
		return -EFAULT;
	loop_info64_from_old(&info, &info64);
	return loop_set_status(lo, &info64);
}

	static int
loop_set_status64(struct loop_device *lo, const struct loop_info64 __user *arg)
{
	struct loop_info64 info64;

	if (copy_from_user(&info64, arg, sizeof (struct loop_info64)))
		return -EFAULT;
	return loop_set_status(lo, &info64);
}

static int
loop_get_status_old(struct loop_device *lo, struct loop_info __user *arg) {
	struct loop_info info;
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_old(&info64, &info);
	if (!err && copy_to_user(arg, &info, sizeof(info)))
		err = -EFAULT;

	return err;
}

static int
loop_get_status64(struct loop_device *lo, struct loop_info64 __user *arg) {
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err && copy_to_user(arg, &info64, sizeof(info64)))
		err = -EFAULT;

	return err;
}

static int loop_set_capacity(struct loop_device *lo, struct block_device *bdev)
{
	int err;
	sector_t sec;
	loff_t sz;

	err = -ENXIO;
	if (unlikely(lo->lo_state != Lo_bound))
		goto out;
	err = figure_loop_size(lo, lo->lo_offset, lo->lo_sizelimit);
	if (unlikely(err))
		goto out;
	sec = get_capacity(lo->lo_disk);
	/* the width of sector_t may be narrow for bit-shift */
	sz = sec;
	sz <<= 9;
	mutex_lock(&bdev->bd_mutex);
	bd_set_size(bdev, sz);
	/* let user-space know about the new size */
	kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	mutex_unlock(&bdev->bd_mutex);

out:
	return err;
}

static int lo_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	int err;

	mutex_lock_nested(&lo->lo_ctl_mutex, 1);
	switch (cmd) {
		case LOOP_SET_FD:
			err = loop_set_fd(lo, mode, bdev, arg);
			break;
		case LOOP_CHANGE_FD:
			err = loop_change_fd(lo, bdev, arg);
			break;
		case LOOP_CLR_FD:
			/* loop_clr_fd would have unlocked lo_ctl_mutex on success */
			err = loop_clr_fd(lo);
			if (!err)
				goto out_unlocked;
			break;
		case LOOP_SET_STATUS:
			err = -EPERM;
			if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN))
				err = loop_set_status_old(lo,
						(struct loop_info __user *)arg);
			break;
		case LOOP_GET_STATUS:
			err = loop_get_status_old(lo, (struct loop_info __user *) arg);
			break;
		case LOOP_SET_STATUS64:
			err = -EPERM;
			if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN))
				err = loop_set_status64(lo,
						(struct loop_info64 __user *) arg);
			break;
		case LOOP_GET_STATUS64:
			err = loop_get_status64(lo, (struct loop_info64 __user *) arg);
			break;
		case LOOP_SET_CAPACITY:
			err = -EPERM;
			if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN))
				err = loop_set_capacity(lo, bdev);
			break;
		default:
			err = lo->ioctl ? lo->ioctl(lo, cmd, arg) : -EINVAL;
	}
	mutex_unlock(&lo->lo_ctl_mutex);

out_unlocked:
	return err;
}

#ifdef CONFIG_COMPAT
struct compat_loop_info {
	compat_int_t	lo_number;      /* ioctl r/o */
	compat_dev_t	lo_device;      /* ioctl r/o */
	compat_ulong_t	lo_inode;       /* ioctl r/o */
	compat_dev_t	lo_rdevice;     /* ioctl r/o */
	compat_int_t	lo_offset;
	compat_int_t	lo_encrypt_type;
	compat_int_t	lo_encrypt_key_size;    /* ioctl w/o */
	compat_int_t	lo_flags;       /* ioctl r/o */
	char		lo_name[LO_NAME_SIZE];
	unsigned char	lo_encrypt_key[LO_KEY_SIZE]; /* ioctl w/o */
	compat_ulong_t	lo_init[2];
	char		reserved[4];
};

/*
 * Transfer 32-bit compatibility structure in userspace to 64-bit loop info
 * - noinlined to reduce stack space usage in main part of driver
 */
	static noinline int
loop_info64_from_compat(const struct compat_loop_info __user *arg,
		struct loop_info64 *info64)
{
	struct compat_loop_info info;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info.lo_number;
	info64->lo_device = info.lo_device;
	info64->lo_inode = info.lo_inode;
	info64->lo_rdevice = info.lo_rdevice;
	info64->lo_offset = info.lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info.lo_encrypt_type;
	info64->lo_encrypt_key_size = info.lo_encrypt_key_size;
	info64->lo_flags = info.lo_flags;
	info64->lo_init[0] = info.lo_init[0];
	info64->lo_init[1] = info.lo_init[1];
	if (info.lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info.lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info.lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info.lo_encrypt_key, LO_KEY_SIZE);
	return 0;
}

/*
 * Transfer 64-bit loop info to 32-bit compatibility structure in userspace
 * - noinlined to reduce stack space usage in main part of driver
 */
	static noinline int
loop_info64_to_compat(const struct loop_info64 *info64,
		struct compat_loop_info __user *arg)
{
	struct compat_loop_info info;

	memset(&info, 0, sizeof(info));
	info.lo_number = info64->lo_number;
	info.lo_device = info64->lo_device;
	info.lo_inode = info64->lo_inode;
	info.lo_rdevice = info64->lo_rdevice;
	info.lo_offset = info64->lo_offset;
	info.lo_encrypt_type = info64->lo_encrypt_type;
	info.lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info.lo_flags = info64->lo_flags;
	info.lo_init[0] = info64->lo_init[0];
	info.lo_init[1] = info64->lo_init[1];
	if (info.lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info.lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info.lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info.lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info.lo_device != info64->lo_device ||
			info.lo_rdevice != info64->lo_rdevice ||
			info.lo_inode != info64->lo_inode ||
			info.lo_offset != info64->lo_offset ||
			info.lo_init[0] != info64->lo_init[0] ||
			info.lo_init[1] != info64->lo_init[1])
		return -EOVERFLOW;

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

	static int
loop_set_status_compat(struct loop_device *lo,
		const struct compat_loop_info __user *arg)
{
	struct loop_info64 info64;
	int ret;

	ret = loop_info64_from_compat(arg, &info64);
	if (ret < 0)
		return ret;
	return loop_set_status(lo, &info64);
}

	static int
loop_get_status_compat(struct loop_device *lo,
		struct compat_loop_info __user *arg)
{
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_compat(&info64, arg);
	return err;
}

static int lo_compat_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	int err;

	switch(cmd) {
		case LOOP_SET_STATUS:
			mutex_lock(&lo->lo_ctl_mutex);
			err = loop_set_status_compat(
					lo, (const struct compat_loop_info __user *) arg);
			mutex_unlock(&lo->lo_ctl_mutex);
			break;
		case LOOP_GET_STATUS:
			mutex_lock(&lo->lo_ctl_mutex);
			err = loop_get_status_compat(
					lo, (struct compat_loop_info __user *) arg);
			mutex_unlock(&lo->lo_ctl_mutex);
			break;
		case LOOP_SET_CAPACITY:
		case LOOP_CLR_FD:
		case LOOP_GET_STATUS64:
		case LOOP_SET_STATUS64:
			arg = (unsigned long) compat_ptr(arg);
		case LOOP_SET_FD:
		case LOOP_CHANGE_FD:
			err = lo_ioctl(bdev, mode, cmd, arg);
			break;
		default:
			err = -ENOIOCTLCMD;
			break;
	}
	return err;
}
#endif

static int lo_open(struct block_device *bdev, fmode_t mode)
{
	struct loop_device *lo;
	int err = 0;

	mutex_lock(&loop_index_mutex);
	lo = bdev->bd_disk->private_data;
	if (!lo) {
		err = -ENXIO;
		goto out;
	}

	mutex_lock(&lo->lo_ctl_mutex);
	lo->lo_refcnt++;
	mutex_unlock(&lo->lo_ctl_mutex);
out:
	mutex_unlock(&loop_index_mutex);
	return err;
}

static int lo_release(struct gendisk *disk, fmode_t mode)
{
	struct loop_device *lo = disk->private_data;
	int err;

	mutex_lock(&lo->lo_ctl_mutex);

	if (--lo->lo_refcnt)
		goto out;

	if (lo->lo_flags & LO_FLAGS_AUTOCLEAR) {
		/*
		 * In autoclear mode, stop the loop thread
		 * and remove configuration after last close.
		 */
		err = loop_clr_fd(lo);
		if (!err)
			goto out_unlocked;
	} else {
		/*
		 * Otherwise keep thread (if running) and config,
		 * but flush possible ongoing bios in thread.
		 */
		loop_flush(lo);
	}

out:
	mutex_unlock(&lo->lo_ctl_mutex);
out_unlocked:
	return 0;
}

static const struct block_device_operations lo_fops = {
	.owner =	THIS_MODULE,
	.open =		lo_open,
	.release =	lo_release,
	.ioctl =	lo_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl =	lo_compat_ioctl,
#endif
};

/*
 * And now the modules code and kernel interface.
 */
static int max_loop;
module_param(max_loop, int, S_IRUGO);
MODULE_PARM_DESC(max_loop, "Maximum number of loop devices");
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per loop device");
MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(LOOP_MAJOR);

int loop_register_transfer(struct loop_func_table *funcs)
{
	unsigned int n = funcs->number;

	if (n >= MAX_LO_CRYPT || xfer_funcs[n])
		return -EINVAL;
	xfer_funcs[n] = funcs;
	return 0;
}

static int unregister_transfer_cb(int id, void *ptr, void *data)
{
	struct loop_device *lo = ptr;
	struct loop_func_table *xfer = data;

	mutex_lock(&lo->lo_ctl_mutex);
	if (lo->lo_encryption == xfer)
		loop_release_xfer(lo);
	mutex_unlock(&lo->lo_ctl_mutex);
	return 0;
}

int loop_unregister_transfer(int number)
{
	unsigned int n = number;
	struct loop_func_table *xfer;

	if (n == 0 || n >= MAX_LO_CRYPT || (xfer = xfer_funcs[n]) == NULL)
		return -EINVAL;

	xfer_funcs[n] = NULL;
	idr_for_each(&loop_index_idr, &unregister_transfer_cb, xfer);
	return 0;
}

EXPORT_SYMBOL(loop_register_transfer);
EXPORT_SYMBOL(loop_unregister_transfer);

static int loop_add(struct loop_device **l, int i)
{
	struct loop_device *lo;
	struct gendisk *disk;
	int err;

	err = -ENOMEM;
	lo = kzalloc(sizeof(*lo), GFP_KERNEL);
	if (!lo)
		goto out;

	if (!idr_pre_get(&loop_index_idr, GFP_KERNEL))
		goto out_free_dev;

	if (i >= 0) {
		int m;

		/* create specific i in the index */
		err = idr_get_new_above(&loop_index_idr, lo, i, &m);
		if (err >= 0 && i != m) {
			idr_remove(&loop_index_idr, m);
			err = -EEXIST;
		}
	} else if (i == -1) {
		int m;

		/* get next free nr */
		err = idr_get_new(&loop_index_idr, lo, &m);
		if (err >= 0)
			i = m;
	} else {
		err = -EINVAL;
	}
	if (err < 0)
		goto out_free_dev;

	lo->lo_queue = blk_alloc_queue(GFP_KERNEL);
	if (!lo->lo_queue)
		goto out_free_dev;

	disk = lo->lo_disk = alloc_disk(1 << part_shift);
	if (!disk)
		goto out_free_queue;

	/*
	 * Disable partition scanning by default. The in-kernel partition
	 * scanning can be requested individually per-device during its
	 * setup. Userspace can always add and remove partitions from all
	 * devices. The needed partition minors are allocated from the
	 * extended minor space, the main loop device numbers will continue
	 * to match the loop minors, regardless of the number of partitions
	 * used.
	 *
	 * If max_part is given, partition scanning is globally enabled for
	 * all loop devices. The minors for the main loop devices will be
	 * multiples of max_part.
	 *
	 * Note: Global-for-all-devices, set-only-at-init, read-only module
	 * parameteters like 'max_loop' and 'max_part' make things needlessly
	 * complicated, are too static, inflexible and may surprise
	 * userspace tools. Parameters like this in general should be avoided.
	 */
	if (!part_shift)
		disk->flags |= GENHD_FL_NO_PART_SCAN;
	disk->flags |= GENHD_FL_EXT_DEVT;
	mutex_init(&lo->lo_ctl_mutex);
	lo->lo_number		= i;
	lo->lo_thread		= NULL;
	init_waitqueue_head(&lo->lo_event);
	init_waitqueue_head(&lo->lo_req_wait);
	spin_lock_init(&lo->lo_lock);
	disk->major		= LOOP_MAJOR;
	disk->first_minor	= i << part_shift;
	disk->fops		= &lo_fops;
	disk->private_data	= lo;
	disk->queue		= lo->lo_queue;
	sprintf(disk->disk_name, "loop%d", i);
	add_disk(disk);
	*l = lo;
	return lo->lo_number;

out_free_queue:
	blk_cleanup_queue(lo->lo_queue);
out_free_dev:
	free_cache(lo);
	kfree(lo);
out:
	return err;
}

static void loop_remove(struct loop_device *lo)
{
	del_gendisk(lo->lo_disk);
	blk_cleanup_queue(lo->lo_queue);
	put_disk(lo->lo_disk);
	//free_cache(lo);
	kfree(lo);
}

static int find_free_cb(int id, void *ptr, void *data)
{
	struct loop_device *lo = ptr;
	struct loop_device **l = data;

	if (lo->lo_state == Lo_unbound) {
		*l = lo;
		return 1;
	}
	return 0;
}

static int loop_lookup(struct loop_device **l, int i)
{
	struct loop_device *lo;
	int ret = -ENODEV;

	if (i < 0) {
		int err;

		err = idr_for_each(&loop_index_idr, &find_free_cb, &lo);
		if (err == 1) {
			*l = lo;
			ret = lo->lo_number;
		}
		goto out;
	}

	/* lookup and return a specific i */
	lo = idr_find(&loop_index_idr, i);
	if (lo) {
		*l = lo;
		ret = lo->lo_number;
	}
out:
	return ret;
}

static struct kobject *loop_probe(dev_t dev, int *part, void *data)
{
	struct loop_device *lo;
	struct kobject *kobj;
	int err;

	mutex_lock(&loop_index_mutex);
	err = loop_lookup(&lo, MINOR(dev) >> part_shift);
	if (err < 0)
		err = loop_add(&lo, MINOR(dev) >> part_shift);
	if (err < 0)
		kobj = ERR_PTR(err);
	else
		kobj = get_disk(lo->lo_disk);
	mutex_unlock(&loop_index_mutex);

	*part = 0;
	return kobj;
}

static long loop_control_ioctl(struct file *file, unsigned int cmd,
		unsigned long parm)
{
	struct loop_device *lo;
	int ret = -ENOSYS;

	mutex_lock(&loop_index_mutex);
	switch (cmd) {
		case LOOP_CTL_ADD:
			ret = loop_lookup(&lo, parm);
			if (ret >= 0) {
				ret = -EEXIST;
				break;
			}
			ret = loop_add(&lo, parm);
			break;
		case LOOP_CTL_REMOVE:
			ret = loop_lookup(&lo, parm);
			if (ret < 0)
				break;
			mutex_lock(&lo->lo_ctl_mutex);
			if (lo->lo_state != Lo_unbound) {
				ret = -EBUSY;
				mutex_unlock(&lo->lo_ctl_mutex);
				break;
			}
			if (lo->lo_refcnt > 0) {
				ret = -EBUSY;
				mutex_unlock(&lo->lo_ctl_mutex);
				break;
			}
			lo->lo_disk->private_data = NULL;
			mutex_unlock(&lo->lo_ctl_mutex);
			idr_remove(&loop_index_idr, lo->lo_number);
			loop_remove(lo);
			break;
		case LOOP_CTL_GET_FREE:
			ret = loop_lookup(&lo, -1);
			if (ret >= 0)
				break;
			ret = loop_add(&lo, -1);
	}
	mutex_unlock(&loop_index_mutex);

	return ret;
}

static const struct file_operations loop_ctl_fops = {
	.open		= nonseekable_open,
	.unlocked_ioctl	= loop_control_ioctl,
	.compat_ioctl	= loop_control_ioctl,
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
};

static struct miscdevice loop_misc = {
	.minor		= LOOP_CTRL_MINOR,
	.name		= "loop-control",
	.fops		= &loop_ctl_fops,
};

MODULE_ALIAS_MISCDEV(LOOP_CTRL_MINOR);
MODULE_ALIAS("devname:loop-control");

static int __init loop_init(void)
{
	int i, nr;
	unsigned long range;
	struct loop_device *lo;
	int err;

	err = misc_register(&loop_misc);
	if (err < 0)
		return err;

	part_shift = 0;
	if (max_part > 0) {
		part_shift = fls(max_part);

		/*
		 * Adjust max_part according to part_shift as it is exported
		 * to user space so that user can decide correct minor number
		 * if [s]he want to create more devices.
		 *
		 * Note that -1 is required because partition 0 is reserved
		 * for the whole disk.
		 */
		max_part = (1UL << part_shift) - 1;
	}

	if ((1UL << part_shift) > DISK_MAX_PARTS)
		return -EINVAL;

	if (max_loop > 1UL << (MINORBITS - part_shift))
		return -EINVAL;

	/*
	 * If max_loop is specified, create that many devices upfront.
	 * This also becomes a hard limit. If max_loop is not specified,
	 * create CONFIG_BLK_DEV_LOOP_MIN_COUNT loop devices at module
	 * init time. Loop devices can be requested on-demand with the
	 * /dev/loop-control interface, or be instantiated by accessing
	 * a 'dead' device node.
	 */
	if (max_loop) {
		nr = max_loop;
		range = max_loop << part_shift;
	} else {
		nr = CONFIG_BLK_DEV_LOOP_MIN_COUNT;
		range = 1UL << MINORBITS;
	}

	if (register_blkdev(LOOP_MAJOR, "loop"))
		return -EIO;

	blk_register_region(MKDEV(LOOP_MAJOR, 0), range,
			THIS_MODULE, loop_probe, NULL, NULL);

	/* pre-create number of devices given by config or max_loop */
	mutex_lock(&loop_index_mutex);
	for (i = 0; i < nr; i++)
		loop_add(&lo, i);
	mutex_unlock(&loop_index_mutex);

	//printk(KERN_INFO "loop: module loaded\n");
	printk (KERN_INFO "LOOP: prefetch_blocks=%dk bound=%d\n",prefetch_blocks,bound);
	printk (KERN_ERR "Loop version 3.0: support sparse files\n");

	prefetch_blocks=prefetch_blocks/4;

	
	return 0;
}

static int loop_exit_cb(int id, void *ptr, void *data)
{
	struct loop_device *lo = ptr;

	loop_remove(lo);
	return 0;
}

static void __exit loop_exit(void)
{
	unsigned long range;

	range = max_loop ? max_loop << part_shift : 1UL << MINORBITS;

	idr_for_each(&loop_index_idr, &loop_exit_cb, NULL);
	idr_remove_all(&loop_index_idr);
	idr_destroy(&loop_index_idr);

	blk_unregister_region(MKDEV(LOOP_MAJOR, 0), range);
	unregister_blkdev(LOOP_MAJOR, "loop");

	misc_deregister(&loop_misc);
}

module_init(loop_init);
module_exit(loop_exit);

#ifndef MODULE
static int __init max_loop_setup(char *str)
{
	max_loop = simple_strtol(str, NULL, 0);
	return 1;
}

__setup("max_loop=", max_loop_setup);
#endif

static inline int cache_hash_func(sector_t nr)
{
	return nr%CACHE_COUNT;
}


static int add_cache(sector_t blknr,sector_t blklocal,struct loop_device *lo)
{
	//printk (KERN_INFO "[BLOCK_CACHE] add_cache blk_local=%d b_blocknr=%d \n",blklocal,blknr);
	struct block_cache_node *node;
	if(atomic_read(&lo -> lru_count) < LRU_LENGTH)
	{
		//printk (KERN_INFO "[BLOCK_CACHE] LRU is not full: LRU_LENGTH=%d lo->lru_count=%d \n",LRU_LENGTH,lo->lru_count);
		node = kmalloc(sizeof(struct block_cache_node),GFP_KERNEL);
		INIT_HLIST_NODE(&(node -> node));
		node -> b_blocknr=0;
		node -> blk_local=0;
		INIT_LIST_HEAD(&node -> next);
		atomic_inc(&lo->lru_count);
	}
	else
	{
		//printk (KERN_INFO "[BLOCK_CACHE] LRU is full: LRU_LENGTH=%d lo->lru_count=%d \n",LRU_LENGTH,lo->lru_count);
		spin_lock_irq (&lo->lru_lock);
		node = list_entry (lo->lru.prev,struct block_cache_node, next);	
		list_del(&node ->next);
		INIT_LIST_HEAD (&node ->next);
		spin_unlock_irq (&lo->lru_lock);
		spin_lock_irq (&lo->cache_lock[cache_hash_func(node->blk_local)]);
		hlist_del(&(node->node));
		INIT_HLIST_NODE(&node->node);
		spin_unlock_irq (&lo->cache_lock[cache_hash_func(node->blk_local)]);
	}

	node -> b_blocknr = blknr;
	node -> blk_local = blklocal;
	spin_lock_irq (&lo->cache_lock[cache_hash_func(node->blk_local)]);
	hlist_add_head( &(node->node),&(lo->block_cache[cache_hash_func(node->blk_local)]) );
	spin_unlock_irq (&lo->cache_lock[cache_hash_func(node->blk_local)]);
	spin_lock_irq (&lo->lru_lock);
	list_add (&node -> next,&lo -> lru);
	spin_unlock_irq (&lo->lru_lock);
	return 0;
}  

static int init_cache(struct loop_device *lo)
{
	int i;
	for(i=0;i<CACHE_COUNT;i++)
	{
		INIT_HLIST_HEAD(&(lo->block_cache[i]));
	}
	return 0;
}

static struct block_cache_node *find_cache_node(sector_t nr, struct loop_device *lo)
{
	struct block_cache_node *pnr;
	struct hlist_node *elem;
	spin_lock_irq (&lo->cache_lock[cache_hash_func(nr)]);
	//printk (KERN_INFO "[find_cache_node] entered hash table lock \n");
	hlist_for_each_entry( pnr, elem, &(lo->block_cache[cache_hash_func(nr)]),node)
		if(pnr -> blk_local == nr)
		{
			//list_del(&pnr -> next);
			//INIT_LIST_HEAD(&pnr->next);
			//list_add_tail (&pnr -> next,&lo ->lru);
			spin_unlock_irq (&lo->cache_lock[cache_hash_func(nr)]);
			//printk (KERN_INFO "[find_cache_node] b_blknr=%lu blk_local=%lu",pnr->b_blocknr,pnr->blk_local);
			return pnr;
		}
	spin_unlock_irq (&lo->cache_lock[cache_hash_func(nr)]);
	return NULL;
}

static int free_cache(struct loop_device *lo)
{
	struct list_head *cur= lo->lru.next;
	struct block_cache_node *cur_node;
	while(cur!=NULL)
	{
		cur_node = list_entry (cur,struct block_cache_node,next);
		cur =(*cur).next;
		hlist_del (&cur_node ->node);
		list_del ((*cur).prev);
		kfree (cur_node);
	}
	return 0;
}

