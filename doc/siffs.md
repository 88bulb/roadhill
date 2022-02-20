# Simple In-File File System

`siffs`是开发剧本杀网关音箱时产生的一个组件。在这个具体的项目里需要缓冲多媒体文件；和git或其它对象存储实现一样，理论上使用hash命名路径的文件池即可工作。

但实际上对于深嵌入式系统这很困难，包括处理器性能不强的低端Linux系统，问题有这样一些：

1. 系统掉电时的文件系统完整性；
   1. 理论上它需要软硬件双重支持，存储设备要有一定的写入保障，比如scsi命令返回时是保证写入非挥发还是只写入了内部的buffer；
   2. 文件系统要有journal支持，发生掉电时要replay write log；象ext4, ntfs都有很好的journal支持，但fat家族，包括fat32和最新的exfat都没有该支持；
2. 使用hash命名路径只能保证文件的查找速度较快，但是遍历性能在低端设备上是极差的；当需要基于LRU（Least Recently Used）算法清理文件时，找到最该被清理的文件需要很久的时间，实际上单单建立几十万的目录和空文件在MCU系统上就需要10-20分钟的时间。LRU本身也需要数据库存储，至少是集中存储和连续读取，而不简单是读取每个文件的时间戳，而且时间戳的维护本身也有其它问题，包括需要网络对时，离线时的时间维护，硬件的实时钟电路支持，电池等等。

基于上述问题，项目里拟设计和实现本文档描述的简单文件内文件系统（`siffs`）。`siffs`做下述简化设计：

1. 使用单一文件存储；
2. 文件写入完成校验正确后对外是只读的；
3. 使用等大小的文件块存储文件，类似于block device的block size，但是针对存储较大文件，不考虑要极致的空间使用率，块的大小较大；
4. 使用bitmap表示allocation，且该allocation map在启动时即时创建；它类似exFAT的设计，区别只是没有写入非挥发介质；
5. 存在一个以hash索引的file metadata table，类似目录功能；该表使用等长的slot，每个slot对应一个hash group，例如完整md5值的为`c195896381ca16c278995d23cb0b79ec`的文件存储在`c195` slot内，实际上该slot可以直接根据record size计算出offset访问，不需要使用key value方式存储；
6. 每个file metadata slot存储hash值头部相同的若干文件的metadata，
   1. metadata里补齐文件的完整hash，例如16字节的md5，分配给slot address，metadata里要包村剩余的14字节；
   2. metadata里有文件的大小8；
   3. metadata里有计数器8记录其最后使用，该计数器是全局计数器且单向增长，写入和（部分）读取都会更新，可以设计不更新该计数器的读取api。
   4. metadata里有使用的文件块记录。8
7. 每个文件的文件块分配遵循两个原则1）顺序分配，2）最多一个hole；
   1. 分配文件空间时必须使用连续和顺序分配原则，即2,3,4,5,6是合法的分配，2,3,5,6,7不是，3,2也不是；
   2. 当空间不足时，简化的实现是通过通过删除旧文件（计数器值最小的）来回收空间，从而找到可用的连续空间；
   3. 但如果新加入的单一文件较大，基于计数器逆序删除文件就不是一个好办法，可能要删除海量文件才能腾出一段足够大的连续空间，如果不能移动文件这完全是几率算法；
   4. 设计文件允许有一个hole，可以在block 2未使用时，把位于3,4,5,6的文件移动到2,3,4,5，这样容易通过“挪动”文件找到更大的空间，而每次挪动一块文件后更新metadata中的hole，可以让文件临时具有一个hole；但有hole的文件应该认定为操作未完成，应尽可能完成该操作而不是让大量的文件都有hole。hole里的空间不可被其它文件分配，也不可以在创建新文件时就创建有hole的文件。
8. 无论写入读取都是串行的，一次执行一个操作，读取之前，更新write log记录之前，更新metadata slot之前，都必须先flush，保证数据一致；
9. 需要实现一个write log；write log长度和slot长度是一样的，都是emmc的一个block大小，512字节，但slot不包含hash的头部，write log里则必须记录该值，所以slot的编码不能占慢，且头部需要留下slot本身内容的hash和slot address的空间。

## 详细设计

基础数据结构（metadata）16个字节。

```C
typedef __attribute__((__packed__)) struct {
    uint8_t md5[16];
    uint32_t size;
    uint32_t access;
    uint32_t start;
    uint16_t hole_start;
    uint16_t hole_size;
} iffs_file_metadata;
```

每个metadata slot包含16个metadata实例；因为不存在zero sized文件，可以根据size直接判定一个struct是不是被占用。

write log是两个一致的metadata slot，即1k大小。无论metadata还是write log均不包含crc或hash计算。

file layout

|       | d           |
| ----------- | ----------- |
| 0-32M       | file header |
| 32-64M      | meta table  |


