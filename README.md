# MultiLanes
MultiLanes is a virtualized storage system for operating system level virtualization on many-core platforms that arms to eliminate the performance interference caused by the shared I/O stack between co-located containers.
Specicially, MultiLanes builds an isolated I/O stack for each container on top of a light-weight virtualized storage device. 
The proposed virtualized storage device enables us to run multiple guest file systems on a single physical device, which is similar to the hardware virtualization schemes.
The key challenges to MultiLanes are how to mitigate the virtualization overhead to achieve near-native performance and how to scale the number of virtualized devices on the host file system which itself scales poorly.
To this end, we propose a set of techniques such as the bypass strategy and constraining the translation threads to a small set of cores.

Specifically, the virtualized device driver of MultiLanes adopts a synchronous bypass strategy to complete I/O requests, thus eliminating the virtualization overhead, and constrains the translation threads that need to interact with the host file system to a small set of cores to avoid contention on the host.
Moreover, a prefeteching mechanism is proposed in MultiLanes to mitigate the communication overhead between the storage devices and translation threads.
The virtualized device driver of MultiLanes is implemented based on the Linux loop device driver.

For more details, please refer to our FAST conference paper [MultiLanes: Providing Virtualized Storage
for OS-level Virtualization on Many Cores](https://www.usenix.org/system/files/conference/fast14/fast14-paper_kang.pdf)<bf /> and ACM TOS article [MultiLanes: Providing Virtualized Storage
for OS-level Virtualization on Many Cores](http://dl.acm.org/citation.cfm?id=2801155&dl=ACM).

#How to run MultiLanes
To run MultiLanes, you should first create sparse files on the host ext3 file system, truncate these files to the predefined size, and then mount the virtualized devices of MultiLanes in the same way how the loop driver works (Our driver is implemented based on loop).
Specifically, you should run command starting with: mount -o loop 

