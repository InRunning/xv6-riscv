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
#define IPB           (BSIZE / sizeof(struct dinode))

// 包含i节点i的块
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// 每块的位图位数
#define BPB           (BSIZE*8)

// 包含块b的对应位的空闲位图块
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// 目录是一个包含一系列dirent结构的文件
#define DIRSIZ 14

// name字段可以有DIRSIZ个字符，并且不以NUL字符结尾
struct dirent {
  ushort inum;
  char name[DIRSIZ] __attribute__((nonstring));
};

