#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xae19f8e, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0x3a45b913, __VMLINUX_SYMBOL_STR(kmem_cache_destroy) },
	{ 0x2eba7c37, __VMLINUX_SYMBOL_STR(kmalloc_caches) },
	{ 0xd2b09ce5, __VMLINUX_SYMBOL_STR(__kmalloc) },
	{ 0x757cd05e, __VMLINUX_SYMBOL_STR(drop_nlink) },
	{ 0x10fbec85, __VMLINUX_SYMBOL_STR(generic_getxattr) },
	{ 0x4c4fef19, __VMLINUX_SYMBOL_STR(kernel_stack) },
	{ 0x699727c6, __VMLINUX_SYMBOL_STR(generic_file_llseek_size) },
	{ 0x60a13e90, __VMLINUX_SYMBOL_STR(rcu_barrier) },
	{ 0xbd100793, __VMLINUX_SYMBOL_STR(cpu_online_mask) },
	{ 0xb9ef5ccb, __VMLINUX_SYMBOL_STR(vfs_llseek) },
	{ 0x52cbb014, __VMLINUX_SYMBOL_STR(lockref_get) },
	{ 0xa028caa1, __VMLINUX_SYMBOL_STR(dput) },
	{ 0xa4ef2f99, __VMLINUX_SYMBOL_STR(inc_nlink) },
	{ 0x52cebcdf, __VMLINUX_SYMBOL_STR(dentry_open) },
	{ 0x1c200925, __VMLINUX_SYMBOL_STR(mutex_unlock) },
	{ 0x85df9b6c, __VMLINUX_SYMBOL_STR(strsep) },
	{ 0x47b62210, __VMLINUX_SYMBOL_STR(vfs_fsync) },
	{ 0x968588d7, __VMLINUX_SYMBOL_STR(generic_read_dir) },
	{ 0xc1b5235a, __VMLINUX_SYMBOL_STR(mount_nodev) },
	{ 0x91715312, __VMLINUX_SYMBOL_STR(sprintf) },
	{ 0xc499ae1e, __VMLINUX_SYMBOL_STR(kstrdup) },
	{ 0x7f21959e, __VMLINUX_SYMBOL_STR(d_delete) },
	{ 0x3d0948eb, __VMLINUX_SYMBOL_STR(vfs_read) },
	{ 0xa7a2ad4e, __VMLINUX_SYMBOL_STR(kern_path) },
	{ 0xfb578fc5, __VMLINUX_SYMBOL_STR(memset) },
	{ 0x77176864, __VMLINUX_SYMBOL_STR(current_task) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0x20c55ae0, __VMLINUX_SYMBOL_STR(sscanf) },
	{ 0x849c72e4, __VMLINUX_SYMBOL_STR(write_inode_now) },
	{ 0x287809c8, __VMLINUX_SYMBOL_STR(vfs_getxattr) },
	{ 0x27a4cdaa, __VMLINUX_SYMBOL_STR(d_move) },
	{ 0xbe93ab60, __VMLINUX_SYMBOL_STR(set_cpus_allowed_ptr) },
	{ 0x449486e4, __VMLINUX_SYMBOL_STR(set_nlink) },
	{ 0x3a761b2f, __VMLINUX_SYMBOL_STR(mutex_lock) },
	{ 0x38c9e7b9, __VMLINUX_SYMBOL_STR(setattr_copy) },
	{ 0x119af014, __VMLINUX_SYMBOL_STR(cpu_bit_bitmap) },
	{ 0x79110d62, __VMLINUX_SYMBOL_STR(unlock_page) },
	{ 0x8926de36, __VMLINUX_SYMBOL_STR(fput) },
	{ 0x11e88e39, __VMLINUX_SYMBOL_STR(kmem_cache_alloc) },
	{ 0x88f0bce3, __VMLINUX_SYMBOL_STR(d_alloc) },
	{ 0x93fca811, __VMLINUX_SYMBOL_STR(__get_free_pages) },
	{ 0xf0fdf6cb, __VMLINUX_SYMBOL_STR(__stack_chk_fail) },
	{ 0xf8e2a085, __VMLINUX_SYMBOL_STR(d_drop) },
	{ 0x2f201aad, __VMLINUX_SYMBOL_STR(vfs_statfs) },
	{ 0x6f20960a, __VMLINUX_SYMBOL_STR(full_name_hash) },
	{ 0x3f0a0987, __VMLINUX_SYMBOL_STR(path_put) },
	{ 0xd52bf1ce, __VMLINUX_SYMBOL_STR(_raw_spin_lock) },
	{ 0xd2cf2c77, __VMLINUX_SYMBOL_STR(kmem_cache_create) },
	{ 0x5516e94c, __VMLINUX_SYMBOL_STR(register_filesystem) },
	{ 0x4302d0eb, __VMLINUX_SYMBOL_STR(free_pages) },
	{ 0xe953b21f, __VMLINUX_SYMBOL_STR(get_next_ino) },
	{ 0x9c934de0, __VMLINUX_SYMBOL_STR(iput) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x87c5697d, __VMLINUX_SYMBOL_STR(d_find_any_alias) },
	{ 0x69acdf38, __VMLINUX_SYMBOL_STR(memcpy) },
	{ 0x2af22aa1, __VMLINUX_SYMBOL_STR(d_splice_alias) },
	{ 0x67f0f162, __VMLINUX_SYMBOL_STR(d_make_root) },
	{ 0x4cbbd171, __VMLINUX_SYMBOL_STR(__bitmap_weight) },
	{ 0x80de7ec8, __VMLINUX_SYMBOL_STR(unregister_filesystem) },
	{ 0xf8fa841b, __VMLINUX_SYMBOL_STR(new_inode) },
	{ 0x1a8ac003, __VMLINUX_SYMBOL_STR(notify_change) },
	{ 0x3bc5f03d, __VMLINUX_SYMBOL_STR(vfs_setxattr) },
	{ 0x3ca256ef, __VMLINUX_SYMBOL_STR(d_instantiate) },
	{ 0x32b776ad, __VMLINUX_SYMBOL_STR(clear_nlink) },
	{ 0xbb6b1263, __VMLINUX_SYMBOL_STR(vfs_write) },
	{ 0x12166dc6, __VMLINUX_SYMBOL_STR(vfs_fsync_range) },
	{ 0xcd5efd7b, __VMLINUX_SYMBOL_STR(generic_fillattr) },
	{ 0xe914e41e, __VMLINUX_SYMBOL_STR(strcpy) },
	{ 0x39424138, __VMLINUX_SYMBOL_STR(generic_shutdown_super) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "43BD325BEE5A5E3C75124B3");
