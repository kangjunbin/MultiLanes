ifneq ($(KERNELRELEASE),)
	obj-m := sfs.o
	sfs-objs = super.o inode.o dentry.o namei.o rename.o dir.o file.o symlink.o dispatch.o kv.o 
else 
#	KERNELDIR ?= /usr/src/linux-3.11.8
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	rm -r *.ko *.mod.c *.o .*.cmd *.order *.symvers
endif	
