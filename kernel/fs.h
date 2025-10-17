// 磁盘文件系统格式。
// 内核和用户程序都使用这个头文件。


#define ROOTINO  1   // 根目录的i节点号
#define BSIZE 1024  // 块大小

// 磁盘布局：
// [ 引导块 | 超级块 | 日志 | i节点块 |
//                                          空闲位图 | 数据块]
//
// mkfs计算超级块并构建初始文件系统。超级块描述了磁盘布局：
struct superblock {
  uint magic;        // 必须是FSMAGIC
  uint size;         // 文件系统映像大小（块数）
  uint nblocks;      // 数据块数量
  uint ninodes;      // i节点数量
  uint nlog;         // 日志块数量
  uint logstart;     // 第一个日志块的块号
  uint inodestart;   // 第一个i节点块的块号
  uint bmapstart;    // 第一个空闲位图块的块号
};

#define FSMAGIC 0x10203040

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// 磁盘上的i节点结构
struct dinode {
  short type;           // 文件类型
  short major;          // 主设备号（仅T_DEVICE类型）
  short minor;          // 次设备号（仅T_DEVICE类型）
  short nlink;          // 文件系统中指向该i节点的链接数
  uint size;            // 文件大小（字节）
  uint addrs[NDIRECT+1];   // 数据块地址
};

// 每块中的i节点数量
// IPB: Inodes Per Block（每块中的 inode 数量）
// 计算逻辑：
// - BSIZE: 磁盘块大小（1024字节）
// - sizeof(struct dinode): 每个磁盘 inode 结构的大小（64字节）
// - IPB = BSIZE / sizeof(struct dinode) = 1024 / 64 = 16
// 表示每个磁盘块可以存储16个 inode
#define IPB           (BSIZE / sizeof(struct dinode))

// 包含i节点i的块
//
// 计算逻辑：
// 1. (i) / IPB - 计算i节点i所在的块索引
//    - i: i节点号（从1开始，0不使用）
//    - IPB: 每个磁盘块包含的i节点数量（BSIZE/sizeof(struct dinode)）
//    - 除法运算确定i节点i位于哪个磁盘块中
//
// 2. + sb.inodestart - 加上i节点区域的起始块号
//    - sb.inodestart: 超级块中定义的i节点区域在磁盘上的起始块号
//    - 这个偏移量将相对块索引转换为绝对磁盘块号
//
// 示例：
// 假设：
// - BSIZE = 1024字节（块大小）
// - sizeof(struct dinode) = 64字节（每个i节点大小）
// - IPB = 1024/64 = 16（每块包含16个i节点）
// - sb.inodestart = 32（i节点区域从磁盘块32开始）
//
// 计算i节点号50所在的块：
// - 50 / 16 = 3（商，表示第4个块，从0开始计数）
// - 3 + 32 = 35（绝对磁盘块号）
//
// 因此，i节点50位于磁盘块35中
//
// 使用场景：
// - 在ialloc()中分配新i节点时
// - 在iupdate()中更新i节点到磁盘时
// - 在ilock()中从磁盘读取i节点时
// - 在ireclaim()中回收孤立i节点时
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// 每块的位图位数
// BPB: Blocks Per Bitmap block（每个位图块管理的块数）
// 计算逻辑：BSIZE * 8 = 每个块的字节数 * 每字节的位数 = 每个块的总位数
// 每个位对应一个数据块的占用状态（1=已使用，0=空闲）
#define BPB           (BSIZE*8)

// 包含块b的对应位的位图块（Bitmap Block）
// 计算逻辑：
// 1. (b) / BPB - 计算块号b所在的位图块索引
//    - b: 数据块号（从0开始）
//    - BPB: 每个位图块管理的块数（BSIZE*8）
//    - 除法运算确定数据块b的位位于哪个位图块中
//
// 2. + sb.bmapstart - 加上位图区域的起始块号
//    - sb.bmapstart: 超级块中定义的位图区域在磁盘上的起始块号
//    - 这个偏移量将相对位图块索引转换为绝对磁盘块号
//
// 示例：
// 假设：
// - BSIZE = 1024字节（块大小）
// - BPB = 1024*8 = 8192（每个位图块管理8192个数据块）
// - sb.bmapstart = 64（位图区域从磁盘块64开始）
//
// 计算数据块10000的位图块：
// - 10000 / 8192 = 1（商，表示第2个位图块，从0开始计数）
// - 1 + 64 = 65（绝对磁盘块号）
//
// 因此，数据块10000的占用状态位位于磁盘块65的位图中
//
// 使用场景：
// - 在balloc()中分配新数据块时
// - 在bfree()中释放数据块时
// - 在bread()中检查数据块是否空闲时
// - 在磁盘空间管理中跟踪块使用状态
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// 目录是一个包含一系列dirent结构的文件
#define DIRSIZ 14

// name字段可以有DIRSIZ个字符，并且不以NUL字符结尾
struct dirent {
  ushort inum;
  char name[DIRSIZ] __attribute__((nonstring));
};
