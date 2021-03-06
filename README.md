# MultiLanes
MultiLanes is a virtualized storage system for operating system level virtualization on many-core platforms that arms to eliminate the performance interference caused by the shared I/O stack between co-located containers.
Specicially, MultiLanes builds an isolated I/O stack for each container on top of a light-weight virtualized storage device, which consists of the virtualized storage device and the partitioned VFS. 
The proposed virtualized storage device enables us to run multiple guest file systems on a single physical device, which is similar to the hardware virtualization schemes.
The partitioned VFS (pVFS) provides a private VFS abstration for each container to eliminate the contention within the VFS layer.
The pVFS is implemented as modifications to the original VFS layer.
We also propose a partitioned page-cache layer for MultiLanes to eliminate the contention within the page cache layer between containers.
The partitioned page-cache layer is implemented as a patch to the cgroup page cache controller to partition the zone LRU lock.

The key challenges to MultiLanes are how to mitigate the virtualization overhead to achieve near-native performance and how to scale the number of virtualized devices on the host file system which itself scales poorly.
To this end, we propose a set of techniques such as the bypass strategy and constraining the translation threads to a small set of cores.

Specifically, the virtualized device driver of MultiLanes adopts a synchronous bypass strategy to complete I/O requests, thus eliminating the virtualization overhead, and constrains the translation threads that need to interact with the host file system to a small set of cores to avoid contention on the host.
Moreover, a prefeteching mechanism is proposed in MultiLanes to mitigate the communication overhead between the storage devices and translation threads.
The virtualized device driver of MultiLanes is implemented based on the Linux loop device driver.

SFS is a stackable file system that stacks a unifing namespace on top of multiple virtualized devices of MultiLanes to mitigate the contention within each single container.
SFS is designed as an extension to MultiLanes, which has been described in our ACM TOS paper [MultiLanes: Providing Virtualized Storage
for OS-level Virtualization on Many Cores](http://dl.acm.org/citation.cfm?id=2801155&dl=ACM).

For more details, please refer to our FAST conference paper [MultiLanes: Providing Virtualized Storage
for OS-level Virtualization on Many Cores](https://www.usenix.org/system/files/conference/fast14/fast14-paper_kang.pdf)<bf /> and ACM TOS article [MultiLanes: Providing Virtualized Storage
for OS-level Virtualization on Many Cores](http://dl.acm.org/citation.cfm?id=2801155&dl=ACM).

If you use our work, please cite our USENIX FAST paper and ACM TOS article.

@inproceedings{multilanes,

  title={MultiLanes: providing virtualized storage for OS-level virtualization on many cores},
  
  author={Kang, Junbin and Zhang, Benlong and Wo, Tianyu and Hu, Chunming and Huai, Jinpeng},
  
  booktitle={Proceedings of the 12th USENIX Conference on File and Storage Technologies (FAST 14)},
  
  pages={317--329},
  
  year={2014}
  
}

@article{kang2016multilanes,

title={MultiLanes: Providing Virtualized Storage for OS-Level Virtualization on Manycores},

author={Kang, Junbin and Hu, Chunming and Wo, Tianyu and Zhai, Ye and Zhang, Benlong and Huai, Jinpeng},

journal={ACM Transactions on Storage},

volume={12},

number={3},

pages={12:1--12:31},

year={2016}

}
    


# How to run MultiLanes
To run MultiLanes, you should first create sparse files on the host ext3 file system, truncate these files to the predefined size, and then mount the virtualized devices of MultiLanes in the same way how the loop driver works (Our driver is implemented based on loop).
Specifically, you should run command starting with: mount -o loop 

We also release the MultiLanes driver alone without pVFS for 3.10.103 kernel (2016/11/15).
You can add the MultiLanes driver code to Linux 3.10.103 (replacing the loop.c, loop.h and adding the block_cache.h) and the ext3 code modified to export ext3_get_block to fs/ext3, enable the loop driver and then compile the kernel.
Note that the MultiLanes driver alone always contains the latest patches fixing the bugs. 

To run MultiLanes driver, you can execute the following steps:

(1) Format the physical device with the modified ext3 file system and mount the device.

           sudo mkfs.ext3 /dev/ssd
           sudo mount -t ext3 /dev/ssd /multilanes/backend/
    
(2) Create the image files on the ext3 file system and truncate the image files to the storage size you required.

           sudo truncate -s 8G /multilanes/backend/image
    
(3) Format the image files with any file system (such as ext3/4 and xfs), and then mount the image files

           sudo mkfs.ext4 -F /multilanes/backend/image
           sudo mount -o loop /multilanes/backend/imgage /vd

(4) Run workloads inside the mountpoint /vd


# Authors
Junbin Kang

Benlong Zhang

Ye Zhai
