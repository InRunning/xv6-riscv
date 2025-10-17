// 文件系统实现，包含五个层次：
//   + 块层：原始磁盘块的分配器
//   + 日志层：多步骤更新的崩溃恢复机制
//   + 文件层：inode 分配器，读写操作，元数据管理
//   + 目录层：具有特殊内容的 inode（其他 inode 的列表）
//   + 名称层：路径如 /usr/rtm/xv6/fs.c，便于命名
//
// 本文件包含底层的文件系统操作例程。
// （更高层的）系统调用实现在 sysfile.c 中。

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// 每个磁盘设备应该有一个超级块，但我们只运行一个设备
struct superblock sb;

// 读取超级块
// 超级块包含文件系统的元数据，如大小、inode 数量等
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  // 读取块 1，其中包含超级块
  bp = bread(dev, 1);
  // 将超级块数据复制到提供的结构体中
  memmove(sb, bp->data, sizeof(*sb));
  // 释放缓冲区
  brelse(bp);
}

// 初始化文件系统
void fsinit(int dev)
{
  // 读取超级块
  readsb(dev, &sb);
  // 检查文件系统魔数，确保这是一个有效的文件系统
  if (sb.magic != FSMAGIC)
    panic("invalid file system");
  // 初始化日志系统
  initlog(dev, &sb);
  // 回收孤立的 inode
  ireclaim(dev);
}

// 将一个块清零
// 用于初始化新分配的块
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  // 读取指定的块
  bp = bread(dev, bno);
  // 将块内容清零
  memset(bp->data, 0, BSIZE);
  // 将更改写入日志
  log_write(bp);
  // 释放缓冲区
  brelse(bp);
}

// 块管理

// 分配一个已清零的磁盘块
// 如果磁盘空间不足，返回 0
static uint
balloc(uint dev)
{
  int b, bi, m;      // b(Block Number 块号)、bi(Bitmap Index 位图索引)、m(Mask 掩码)
  struct buf *bp;    // bp(Buffer Pointer 缓冲区指针)

  bp = 0;
  // 遍历所有块，每次处理一个位图块（BPB 个块）
  for (b = 0; b < sb.size; b += BPB)
  {
    // 读取包含块 b 的位图块
    bp = bread(dev, BBLOCK(b, sb));
    // 遍历位图块中的每个位
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++)
    {
      // 计算位掩码
      m = 1 << (bi % 8);
      // 检查块是否空闲（位为 0）
      if ((bp->data[bi / 8] & m) == 0)
      {
        // 标记块为已使用
        bp->data[bi / 8] |= m;
        // 将更改写入日志
        log_write(bp);
        // 释放缓冲区
        brelse(bp);
        // 清零新分配的块
        bzero(dev, b + bi);
        // 返回块号
        return b + bi;
      }
    }
    // 释放位图块缓冲区
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// 释放一个磁盘块
static void
bfree(int dev, uint b)
{
  struct buf *bp; // bp(Buffer Pointer 缓冲区指针)
  int bi, m;      // bi(Bitmap Index 位图索引)、m(Mask 掩码)

  // 位图结构说明：
  //   - 磁盘按 BPB (Blocks Per Bitmap block) 为单位划分，每个 bit 描述一个数据块的占用状态；
  //   - `BBLOCK(b, sb)` 给出块号 b 对应的位图所在磁盘块号；等价于 (b / BPB) + bmapstart
  //   - `bi` 表示 b 在该位图块内的序号，`bi/8` 找到具体字节，`bi%8` 决定字节中的具体比特位。
  //
  // 释放流程：
  //   1. 读出位图块；
  //   2. 检查目标 bit 是否已经清零（若已为 0，说明重复释放，直接 panic）；
  //   3. 置 0 后调用 `log_write` 把修改记入日志，以保证事务一致性；
  //   4. 释放缓冲区。
  // 读取包含块 b 的位图块
  bp = bread(dev, BBLOCK(b, sb)); // bp(Buffer Pointer 缓冲区指针) 指向位图块的缓存
  // 每个 bit 对应一个物理块：先计算目标块在位图中的编号（bi），再定位它所在的字节和比特位。
  // 计算块在位图中的索引
  bi = b % BPB;                   // bi(Bitmap Index 位图索引) 表示目标块在该位图块中的序号
  // 计算位掩码
  m = 1 << (bi % 8);              // m(Mask 掩码) 定位目标比特位
  // 检查块是否已经是空闲的
  if ((bp->data[bi / 8] & m) == 0)
    panic("freeing free block");
  // 清除位，标记块为空闲
  bp->data[bi / 8] &= ~m;
  // 将更改写入日志
  log_write(bp);
  // 释放缓冲区
  brelse(bp);
}

// Inodes (索引节点)
//
// 一个 inode 描述一个无名的文件。
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

// inode 表结构
// 包含一个自旋锁和一组 inode 缓存
struct
{
  struct spinlock lock;       // 保护 inode 表的自旋锁
  struct inode inode[NINODE]; // inode 缓存数组
} itable;

// 初始化 inode 表
void iinit()
{
  int i = 0;

  // 初始化 inode 表的自旋锁
  initlock(&itable.lock, "itable");
  // NINODE 定义了 inode 缓存中 inode 的数量，目前是一个常量 50
  for (i = 0; i < NINODE; i++)
  {
    // 为每个 inode 初始化一个睡眠锁。
    // 睡眠锁允许持有锁的进程在等待资源时进入睡眠状态，
    // 从而避免忙等待，提高 CPU 利用率。
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode *iget(uint dev, uint inum);

// 在设备 dev 上分配一个 inode
// 通过给它类型 type 来标记为已分配
// 返回一个已分配但未锁定且已被引用的 inode，
// 如果没有空闲 inode，则返回 NULL
struct inode *
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  // 遍历所有 inode（跳过 inode 0，它不被使用）
  for (inum = 1; inum < sb.ninodes; inum++)
  {
    // 读取包含 inode inum 的块
    bp = bread(dev, IBLOCK(inum, sb));
    // 获取指向 inode inum 的指针
    dip = (struct dinode *)bp->data + inum % IPB;
    // 检查 inode 是否空闲（类型为 0）
    if (dip->type == 0)
    {
      // 找到一个空闲 inode
      memset(dip, 0, sizeof(*dip)); // 清零 inode
      dip->type = type;             // 设置类型
      log_write(bp);                // 在磁盘上标记为已分配
      brelse(bp);                   // 释放缓冲区
      // 获取 inode 的内存表示并增加引用计数
      return iget(dev, inum);
    }
    brelse(bp); // 释放缓冲区
  }
  printf("ialloc: no inodes\n");
  return 0;
}

// 将修改后的内存 inode 复制到磁盘
// 在每次修改存储在磁盘上的 ip->xxx 字段后必须调用
// 调用者必须持有 ip->lock
void iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  // 读取包含 inode 的块
  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  // 获取指向磁盘 inode 的指针
  dip = (struct dinode *)bp->data + ip->inum % IPB;
  // 复制 inode 的元数据
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  // 复制块地址数组
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  // 将更改写入日志
  log_write(bp);
  // 释放缓冲区
  brelse(bp);
}

// 在设备 dev 上查找编号为 inum 的 inode
// 并返回其内存副本。不锁定 inode，
// 也不从磁盘读取它。
static struct inode *
iget(uint dev, uint inum)
{
  struct inode *ip, *empty; // ip为inode pointer(索引节点指针)缩写，empty指向空闲槽位

  // 获取 inode 表锁
  acquire(&itable.lock);

  // inode 是否已经在表中？
  empty = 0;
  // 遍历 inode 表
  for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++)
  {
    // 检查是否是我们要找的 inode
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
    {
      // 找到了，增加引用计数
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    // 记住第一个空闲槽位
    if (empty == 0 && ip->ref == 0)
      empty = ip;
  }

  // 回收一个 inode 条目
  if (empty == 0)
    panic("iget: no inodes");

  // 使用空闲槽位
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0; // 标记为无效，需要从磁盘读取
  release(&itable.lock);

  return ip;
}

// 增加 ip 的引用计数
// 返回 ip 以支持 ip = idup(ip1) 的用法
struct inode *
idup(struct inode *ip) // ip为inode pointer(Index Node 指针)的缩写
{
  // 获取 inode 表锁
  acquire(&itable.lock);
  // 增加引用计数
  ip->ref++;
  // 释放 inode 表锁
  release(&itable.lock);
  return ip;
}

// 锁定给定的 inode
// 如果需要，从磁盘读取 inode
void ilock(struct inode *ip)
{
  struct buf *bp;      // bp: Buffer Pointer (缓冲区指针)，指向从磁盘读取的包含目标inode的块的缓存
                       // 用于临时存储从磁盘读取的块数据，后续通过dip访问该块中的特定inode
  struct dinode *dip;  // dip: Disk INode Pointer (磁盘索引节点指针)，指向磁盘上inode的内存表示
                       // 指向从磁盘读取的inode数据，包含文件的元数据（类型、大小、链接数、数据块地址等）

  // 检查 inode 是否有效
  if (ip == 0 || ip->ref < 1)
    panic("ilock");

  // 获取 inode 的睡眠锁
  acquiresleep(&ip->lock);

  // 如果 inode 数据无效，从磁盘读取
  if (ip->valid == 0)
  {
    // 读取包含 inode 的块
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    // 获取指向磁盘 inode 的指针
    // 计算逻辑：
    // 1. (struct dinode *)bp->data - 将缓冲区数据转换为磁盘 inode 数组指针
    //    - bp->data 指向磁盘块的数据区域，包含多个连续的 dinode 结构
    //    - 每个磁盘块包含 IPB 个 inode（BSIZE/sizeof(struct dinode)）
    //
    // 2. ip->inum % IPB - 计算目标 inode 在块内的偏移量
    //    - ip->inum: inode 编号（从1开始，0不使用）
    //    - IPB: 每个磁盘块包含的 inode 数量（BSIZE/sizeof(struct dinode)）
    //    - 取模运算确定目标 inode 在当前块中的位置
    //
    // 示例：
    // 假设：
    // - BSIZE = 1024字节（块大小）
    // - sizeof(struct dinode) = 64字节（每个 inode 大小）
    // - IPB = 1024/64 = 16（每块包含16个 inode）
    // - 目标 inode 编号为 50
    //
    // 计算过程：
    // - 50 % 16 = 2（偏移量，表示第3个 inode，从0开始计数）
    // - 最终指向 bp->data[2] 处的 dinode 结构
    //
    // 内存布局示例：
    // bp->data[0]   -> inode 0-15（如果存在）
    // bp->data[1]   -> inode 16-31
    // bp->data[2]   -> inode 32-47
    // bp->data[3]   -> inode 48-63
    // ...
    // dip 指向目标 inode 的内存表示，包含文件的元数据
    //（类型、大小、链接数、数据块地址等）
    dip = (struct dinode *)bp->data + ip->inum % IPB;
    // 复制 inode 数据
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    // 释放缓冲区
    brelse(bp);
    // 标记为有效
    ip->valid = 1;
    // 检查 inode 类型
    if (ip->type == 0)
      panic("ilock: no type");
  }
}

// 解锁给定的 inode
void iunlock(struct inode *ip)
{
  // 检查 inode 是否有效且当前调用者确实持有 ip->lock 睡眠锁
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  // 释放 inode 的睡眠锁
  releasesleep(&ip->lock);
}

// 减少对内存 inode 的引用
// 如果这是最后一个引用，inode 表条目可以被回收
// 如果这是最后一个引用且 inode 没有链接，
// 则释放磁盘上的 inode（及其内容）
// 所有对 iput() 的调用必须在事务内，
// 以防它需要释放 inode
void iput(struct inode *ip)
{
  // 获取 inode table 锁
  acquire(&itable.lock);

  // 检查是否是最后一个引用且 inode 没有链接（nlink 为目录项引用计数）
  if (ip->ref == 1 && ip->valid && ip->nlink == 0)
  {
    // inode 没有链接且没有其他引用：截断并释放

    // ip->ref == 1 意味着没有其他进程可以锁定 ip，
    // 所以这个 acquiresleep() 不会阻塞（或死锁）
    acquiresleep(&ip->lock);

    // 释放 inode 表锁
    release(&itable.lock);

    // 截断 inode 的内容
    itrunc(ip);
    // 清零类型
    ip->type = 0;
    // 更新磁盘上的 inode
    iupdate(ip);
    // 标记为无效
    ip->valid = 0;

    // 释放 inode 的睡眠锁
    releasesleep(&ip->lock);

    // 重新获取 inode 表锁
    acquire(&itable.lock);
  }

  // 减少引用计数
  ip->ref--;
  // 释放 inode 表锁
  release(&itable.lock);
}

// 常见用法：先解锁，然后释放
void iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// 回收设备上的孤立 inode
// 孤立 inode 是指类型非零但链接计数为 0 的 inode， ireclaim 是inode reclaim的简写
void ireclaim(int dev)
{
  // 遍历所有 inode
  for (int inum = 1; inum < sb.ninodes; inum++)
  {
    struct inode *ip = 0;
    // 读取包含 inode 的块
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    // 获取指向磁盘 inode 的指针
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    // 检查是否是孤立 inode
    if (dip->type != 0 && dip->nlink == 0)
    {
      // 是一个孤立 inode
      printf("ireclaim: orphaned inode %d\n", inum);
      // 获取 inode 的内存表示
      ip = iget(dev, inum);
    }
    // 释放缓冲区
    brelse(bp);
    if (ip)
    {
      // 开始事务
      begin_op();
      // 锁定 inode
      ilock(ip);
      // 立即解锁（我们只需要通过 iput 来释放它）
      iunlock(ip);
      // 释放 inode（这将清理它）
      iput(ip);
      // 结束事务
      end_op();
    }
  }
}

// Inode 内容
//
// 与每个 inode 关联的内容（数据）存储在磁盘上的块中。
// 前 NDIRECT 个块号列在 ip->addrs[] 中。
// 接下来的 NINDIRECT 个块列在块 ip->addrs[NDIRECT] 中。

// 返回 inode ip 中第 bn 个块的磁盘块地址
// 如果没有这样的块，bmap 分配一个
// 如果磁盘空间不足，返回 0
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  // 处理直接块
  if (bn < NDIRECT)
  {
    // 检查直接块是否已分配
    if ((addr = ip->addrs[bn]) == 0)
    {
      // 分配一个新块
      addr = balloc(ip->dev);
      if (addr == 0)
        return 0;
      // 保存块地址
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  // 调整块号，减去直接块数量
  bn -= NDIRECT;

  // 处理间接块
  if (bn < NINDIRECT)
  {
    // 加载间接块，必要时分配
    if ((addr = ip->addrs[NDIRECT]) == 0)
    {
      // 分配间接块
      addr = balloc(ip->dev);
      if (addr == 0)
        return 0;
      // 保存间接块地址
      ip->addrs[NDIRECT] = addr;
    }
    // 读取间接块
    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;
    // 检查间接块中的块是否已分配
    if ((addr = a[bn]) == 0)
    {
      // 分配一个新块
      addr = balloc(ip->dev);
      if (addr)
      {
        // 保存块地址到间接块
        a[bn] = addr;
        // 将更改写入日志
        log_write(bp);
      }
    }
    // 释放缓冲区
    brelse(bp);
    return addr;
  }

  // 块号超出范围
  panic("bmap: out of range");
}

// 截断 inode（丢弃内容）
// 调用者必须持有 ip->lock
void itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  // 释放所有直接块
  for (i = 0; i < NDIRECT; i++)
  {
    if (ip->addrs[i])
    {
      // 释放直接块
      bfree(ip->dev, ip->addrs[i]);
      // 清零块地址
      ip->addrs[i] = 0;
    }
  }

  // 处理间接块
  if (ip->addrs[NDIRECT])
  {
    // 读取间接块
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint *)bp->data;
    // 释放间接块中的所有块
    for (j = 0; j < NINDIRECT; j++)
    {
      if (a[j])
        bfree(ip->dev, a[j]);
    }
    // 释放间接块缓冲区
    brelse(bp);
    // 释放间接块本身
    bfree(ip->dev, ip->addrs[NDIRECT]);
    // 清零间接块地址
    ip->addrs[NDIRECT] = 0;
  }

  // 将文件大小设为 0
  ip->size = 0;
  // 更新磁盘上的 inode
  iupdate(ip);
}

// 从 inode 复制 stat 信息
// 调用者必须持有 ip->lock
void stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;     // 设备号
  st->ino = ip->inum;    // inode 号
  st->type = ip->type;   // 文件类型
  st->nlink = ip->nlink; // 链接计数
  st->size = ip->size;   // 文件大小
}

// 从 inode 读取数据
// 调用者必须持有 ip->lock
// 如果 user_dst==1，则 dst 是用户虚拟地址；
// 否则，dst 是内核地址
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;         // tot(Total 累计已处理字节数)、m(Chunk Size 本次块内处理字节数)
  struct buf *bp;      // bp(Buffer Pointer 缓冲区指针)，指向缓存的磁盘块

  // 检查偏移量是否有效
  if (off > ip->size || off + n < off)
    return 0;
  // 调整读取大小，不超过文件大小
  if (off + n > ip->size)
    n = ip->size - off;

  // 循环读取数据，每次处理一个块
  for (tot = 0; tot < n; tot += m, off += m, dst += m)
  {
    // 获取包含偏移量的块地址
    uint addr = bmap(ip, off / BSIZE);
    if (addr == 0)
      break;
    // 读取块
    bp = bread(ip->dev, addr);
    // 计算本次要读取的字节数
    m = min(n - tot, BSIZE - off % BSIZE);
    // 从块复制数据到目标地址
    if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1)
    {
      // 复制失败
      brelse(bp);
      tot = -1;
      break;
    }
    // 释放缓冲区
    brelse(bp);
  }
  return tot;
}

// 向 inode 写入数据
// 调用者必须持有 ip->lock
// 如果 user_src==1，则 src 是用户虚拟地址；
// 否则，src 是内核地址
// 返回成功写入的字节数
// 如果返回值小于请求的 n，则表示出现了某种错误
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  // 这个辅助函数将原始字节写入 inode 支持的文件。它既理解用户空间缓冲区也理解内核空间缓冲区
  // （由 `user_src` 标志控制），在必要时扩展文件，并将每个脏缓冲区标记为包含在
  // 预写日志中，这样调用者就不需要执行显式的磁盘 I/O。
  //
  // 缩写解释：
  // - `ip`: inode 指针（描述磁盘元数据的索引节点结构）。
  // - `BSIZE`: 块大小（每个文件系统块的字节数）。
  // - `bmap`: 块映射例程，将逻辑块号转换为磁盘扇区地址，并在过程中分配块。
  // - `bp`: 缓冲区指针，包装缓存的磁盘块。
  //
  // 高级步骤：
  // 1. 验证写入不会环绕地址空间或超过每个文件的 `MAXFILE` 块限制。
  // 2. 逐块迭代，使用 `bmap`（块映射）确保后备存储空间存在。
  // 3. 使用 `bread` 将块读入缓存，通过 `either_copyin` 从用户或内核源复制字节，
  //    并使用 `log_write` 将缓冲区标记为脏，以便日志层捕获更改。
  // 4. 释放每个缓冲区，当写入超过之前的文件末尾时更新 inode 大小，
  //    并使用 `iupdate`（inode 更新）持久化 inode 元数据。
  //
  // 返回值反映成功复制的字节数；任何部分失败都会截断循环并报告已完成的字节数前缀。
  uint tot, m;         // tot(Total bytes 累计传输的字节数)、m(Chunk size 本轮处理字节数)
  struct buf *bp;      // bp(Buffer Pointer 缓冲区指针)，指向当前缓存的磁盘块

  // 检查偏移量是否有效
  //
  // 这个检查确保两个关键条件：
  // 1. off > ip->size: 写入起始偏移量不能超过当前文件大小
  //    - 如果从文件末尾之后开始写入，这是不允许的
  //    - 文件系统不支持"空洞"写入，必须连续写入
  //
  // 2. off + n < off: 检查算术溢出
  //    - 当 n 很大时，off + n 可能会溢出，变成一个很小的值
  //    - 例如：off = 0xFFFFFFFF, n = 1, off + n = 0 (溢出)
  //    - 这种情况下，off + n < off 为 true，表示发生了溢出
  //    - 溢出会导致错误的内存访问，必须防止
  //
  // 如果任一条件不满足，返回 -1 表示错误
  if (off > ip->size || off + n < off)
    return -1;
  // 检查是否会超过最大文件大小
  if (off + n > MAXFILE * BSIZE)
    return -1;

  // 循环写入数据，每次处理一个块
  for (tot = 0; tot < n; tot += m, off += m, src += m)
  {
    // 获取包含偏移量的块地址
    // 详细说明：
    // 1. off / BSIZE: 计算逻辑块号
    //    - off: 文件内的字节偏移量（从0开始）
    //    - BSIZE: 每个磁盘块的大小（通常为1024字节）
    //    - 除法运算将字节偏移量转换为块号
    //    例如：如果 off=2048, BSIZE=1024，则块号为 2
    //
    // 2. bmap(ip, block_number): 块映射函数
    //    - ip: 指向文件inode的指针，包含文件的元数据
    //    - block_number: 逻辑块号（由 off/BSIZE 计算得出）
    //    - 功能：将逻辑块号转换为实际的磁盘块地址
    //    - 如果块尚未分配，会自动分配新块
    //    - 支持直接块、间接块和双重间接块的映射
    //
    // 3. 返回值 addr:
    //    - 成功时：返回磁盘块的实际地址（块号）
    //    - 失败时：返回 0（表示磁盘空间不足）
    //
    // 4. 在文件读写中的作用：
    //    - 这是文件系统实现的关键步骤，将逻辑文件位置转换为物理磁盘位置
    //    - 为后续的磁盘I/O操作（bread/bwrite）提供目标地址
    //    - 支持文件的随机访问，可以跳转到任意位置进行读写
    //
    // 5. 示例：
    //    假设文件大小为 3072 字节，BSIZE=1024：
    //    - 写入偏移量 0-1023：访问块 0
    //    - 写入偏移量 1024-2047：访问块 1
    //    - 写入偏移量 2048-3071：访问块 2
    uint addr = bmap(ip, off / BSIZE);
    if (addr == 0)
      break;
    // 读取块
    bp = bread(ip->dev, addr);
    // 计算本次要写入的字节数
    m = min(n - tot, BSIZE - off % BSIZE);
    // 从源地址复制数据到块
    if (either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1)
    {
      // 复制失败
      brelse(bp);
      break;
    }
    // 将更改写入日志
    log_write(bp);
    // 释放缓冲区
    brelse(bp);
  }

  // 如果写入位置超过当前文件大小，更新文件大小
  if (off > ip->size)
    ip->size = off;

  // 即使大小没有改变，也要将 inode 写回磁盘，
  // 因为上面的循环可能调用了 bmap() 并向 ip->addrs[] 添加了新块
  iupdate(ip);

  return tot;
}

// 目录操作

// 比较目录项名称
int namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// 在目录中查找目录项
// 如果找到，设置 *poff 为条目的字节偏移量
struct inode *
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  // 检查 dp 是否是目录
  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  // 遍历目录中的所有条目
  for (off = 0; off < dp->size; off += sizeof(de))
  {
    // 读取目录条目
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    // 跳过空闲条目
    if (de.inum == 0)
      continue;
    // 检查名称是否匹配
    if (namecmp(name, de.name) == 0)
    {
      // 条目匹配路径元素
      if (poff)
        *poff = off;
      inum = de.inum;
      // 返回对应的 inode
      return iget(dp->dev, inum);
    }
  }

  // 未找到
  return 0;
}

// 向目录 dp 写入一个新的目录项 (name, inum)
// 成功返回 0，失败返回 -1（例如磁盘块不足）
int dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip; // ip为inode pointer(Index Node 指针)的缩写

  // 检查名称是否已存在
  if ((ip = dirlookup(dp, name, 0)) != 0)
  {
    iput(ip);
    return -1;
  }

  // 查找一个空的目录条目
  for (off = 0; off < dp->size; off += sizeof(de))
  {
    // 读取目录条目
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    // 找到空闲条目
    if (de.inum == 0)
      break;
  }

  // 设置目录项
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  // 写入目录项
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// 路径解析

// 从路径中复制下一个路径元素到 name
// 返回指向复制元素后面的元素的指针
// 返回的路径没有前导斜杠，
// 所以调用者可以检查 *path=='\0' 来看名称是否是最后一个
// 如果没有名称要移除，返回 0
//
// 示例:
//   skipelem("a/bb/c", name) = "bb/c", 设置 name = "a"
//   skipelem("///a//bb", name) = "bb", 设置 name = "a"
//   skipelem("a", name) = "", 设置 name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *
skipelem(char *path, char *name)
{
  char *s;
  int len;

  // 跳过前导斜杠
  while (*path == '/')
    path++;
  // 如果路径为空，返回 0
  if (*path == 0)
    return 0;
  // 记住元素开始位置
  s = path;
  // 找到下一个斜杠或字符串结尾
  while (*path != '/' && *path != 0)
    path++;
  // 计算元素长度
  len = path - s;
  // 复制元素名称
  if (len >= DIRSIZ)
    // 目录项名超过 DIRSIZ，按照 xv6 规则截断为固定长度
    memmove(name, s, DIRSIZ);
  else
  {
    // 正常长度：复制实际字符并自行补终止符
    memmove(name, s, len); // 将当前路径分量从源指针 s 复制到输出缓冲区 name
    name[len] = 0;
  }
  // 跳过后续斜杠
  while (*path == '/')
    path++;
  return path;
}

// 查找并返回路径名的 inode
// 如果 parent != 0，返回父目录的 inode 并将最终
// 路径元素复制到 name，name 必须有 DIRSIZ 字节的空间
// 必须在事务内调用，因为它调用了 iput()
static struct inode *
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next; // ip为当前inode pointer(Index Node 指针)，next为下一层inode指针

  // 如果路径为空字符串，默认从当前目录开始解析
  // （nameiparent 模式需要依旧走完整个流程来捕获父目录）

  // 如果路径以 '/' 开头，从根目录开始
  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    // 否则从当前工作目录开始
    ip = idup(myproc()->cwd);

  // 逐个处理路径元素，将path复制到name
  while ((path = skipelem(path, name)) != 0)
  {
    // name 现在保存了一个目录项，如 "."、".." 或普通文件名
    // 锁定当前 inode
    ilock(ip);
    // 检查是否是目录
    if (ip->type != T_DIR)
    {
      iunlockput(ip);
      return 0;
    }
    // 如果需要父目录且这是最后一个元素
    if (nameiparent && *path == '\0')
    {
      // 提前停止一级
      iunlock(ip);
      return ip;
    }
    // 在目录中查找下一个元素
    if ((next = dirlookup(ip, name, 0)) == 0)
    {
      // 找不到目标子项，解析失败
      iunlockput(ip);
      return 0;
    }
    // 释放当前 inode，移动到下一个
    iunlockput(ip);
    ip = next;
  }
  // 如果需要父目录但没有找到，释放并返回 0
  if (nameiparent)
  {
    iput(ip);
    return 0;
  }
  // 返回解析到的最终 inode（调用者负责后续锁管理）
  return ip;
}

// 将路径名转换为 inode
struct inode *
namei(char *path)
{
  char name[DIRSIZ];
  // 使用临时缓冲区逐级解析路径，返回最终目标的 inode
  return namex(path, 0, name);
}

// 将路径名转换为父目录 inode 和文件名
//
// 功能：解析路径，返回父目录的 inode，并将最终文件名复制到 name 参数中
// 这个函数在需要创建、重命名或删除文件时特别有用，因为它提供了父目录的 inode
//
// 参数：
//   path - 要解析的路径字符串
//   name - 用于存储最终文件名的缓冲区，必须至少有 DIRSIZ 字节的空间
//
// 返回值：
//   成功时返回父目录的 inode 指针，并将最终文件名复制到 name 参数中
//   失败时返回 NULL（路径不存在或无法访问）
//
// 示例：
//   nameiparent("/usr/rtm/xv6/fs.c", name)
//     - 返回目录 /usr/rtm/xv6 的 inode
//     - 将 "fs.c" 复制到 name 参数中
//
// 注意：
//   - 调用者需要负责后续的 inode 锁定和释放
//   - 此函数不检查文件权限
//   - name 参数必须有足够的空间（至少 DIRSIZ 字节）
//   - 内部调用 namex() 函数，设置 nameiparent=1 来获取父目录
struct inode *
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
