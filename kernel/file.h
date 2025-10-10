// 进程打开文件的内核抽象
struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;  // 打开对象的类型枚举，FD(File Descriptor 文件描述符) 前缀对应具体类别
  int ref;                                              // 当前引用计数(reference count 引用计数值)，用于控制共享文件结构的生命周期
  char readable;                                        // 是否允许读(readable 可读标志)，非零表示可读
  char writable;                                        // 是否允许写(writable 可写标志)，非零表示可写
  struct pipe *pipe;                                    // 当type为FD_PIPE时指向管道(pipe 管道)结构，其余类型为空
  struct inode *ip;                                     // 当type为FD_INODE或FD_DEVICE时指向inode(Index Node 索引节点)对象
  uint off;                                             // 仅FD_INODE有效的文件偏移量(offset 偏移位置)，单位字节
  short major;                                          // 仅FD_DEVICE有效的主设备号(major 主设备号)，用于定位设备驱动
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)              // 提取主设备号(major 主设备号)，高16位表示
#define minor(dev)  ((dev) & 0xFFFF)                    // 提取次设备号(minor 次设备号)，低16位表示
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))              // 由主设备号m与次设备号n组合成单个设备编号

// inode(Index Node 索引节点) 的内存副本
struct inode {
  uint dev;                  // 所属设备号(dev 设备编号)，标识设备种类
  uint inum;                 // inode编号(inode number 节点编号)，磁盘上的唯一标识
  int ref;                   // 引用计数(reference count 引用计数值)，跟踪共享次数
  struct sleeplock lock;     // 睡眠锁(sleeplock 睡眠互斥锁)，保护下方字段
  int valid;                 // 标记是否已从磁盘读取(valid 是否有效)，非零表示缓存内容有效

  short type;                // 磁盘inode类型(type 类型码)，如T_FILE/T_DIR等
  short major;               // 对于设备文件的主设备号(major 主设备号)
  short minor;               // 对于设备文件的次设备号(minor 次设备号)
  short nlink;               // 链接计数(nlink 链接数量)，硬链接总数
  uint size;                 // 文件大小(size 文件长度)，单位字节
  uint addrs[NDIRECT+1];     // 数据块地址数组(addrs 地址数组)，包含直接块和一个间接块指针
};

// 主设备号到设备函数表的映射
struct devsw {
  int (*read)(int, uint64, int);   // 读取函数指针(read 读操作)，参数依次为设备号、用户缓冲区地址、字节数
  int (*write)(int, uint64, int);  // 写入函数指针(write 写操作)，参数与read一致
};

extern struct devsw devsw[];       // 设备交换表(devsw 设备切换表)数组，索引即主设备号

#define CONSOLE 1                  // 控制台设备(console 控制台)的主设备号
