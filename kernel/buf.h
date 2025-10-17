// 缓冲区结构体，用于文件系统的磁盘缓存
// 这个结构体实现了LRU（最近最少使用）缓存算法来管理磁盘块
struct buf {
  int valid;   // 数据有效性标志：是否已从磁盘读取数据？
  int disk;    // 磁盘所有权标志：磁盘是否"拥有"这个缓冲区？
  uint dev;    // 设备号：标识缓冲区所属的设备
  uint blockno; // 块号：缓冲区在设备上的块编号
  struct sleeplock lock; // 睡眠锁：用于同步对缓冲区的访问
  uint refcnt; // 引用计数：记录有多少进程正在使用这个缓冲区
  struct buf *prev; // 前向指针：用于LRU缓存链表
  struct buf *next; // 后向指针：用于LRU缓存链表
  uchar data[BSIZE]; // 数据存储区：存储实际的磁盘块数据
};

