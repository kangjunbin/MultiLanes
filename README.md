# MultiLanes
MultiLanes is a virtualized storage system for operating system level virtualization on many-core platforms that arms to eliminate the performance interference caused by the shared I/O stack between co-located containers.
Specicially, MultiLanes builds an isolated I/O stack for each container on top of a light-weight virtualized storage device. 
The proposed virtualized storage device enables us to run multiple guest file systems on a single physical device, which is similar to the hardware virtualization schemes.
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

#How to run MultiLanes
To run MultiLanes, you should first create sparse files on the host ext3 file system, truncate these files to the predefined size, and then mount the virtualized devices of MultiLanes in the same way how the loop driver works (Our driver is implemented based on loop).
Specifically, you should run command starting with: mount -o loop 

We also release the MultiLanes driver alone without pVFS for 3.10.103 kernel (2016/11/15).
You can patch the MultiLanes driver code to Linux 3.10.103 (replacing the loop.c, loop.h and adding the block_cache.h), enable the loop driver and then compile the kernel.
Then, you can use MultiLanes as the way using loop as our driver is implemented based on loop.

#Authors
Junbin Kang

Benlong Zhang

Ye Zhai
