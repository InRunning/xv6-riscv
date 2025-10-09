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
  int b, bi, m;
  struct buf *bp;

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
  struct buf *bp;
  int bi, m;

  // 读取包含块 b 的位图块
  bp = bread(dev, BBLOCK(b, sb));
  // 计算块在位图中的索引
  bi = b % BPB;
  // 计算位掩码
  m = 1 << (bi % 8);
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
  struct inode *ip, *empty;

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
idup(struct inode *ip)
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
  struct buf *bp;
  struct dinode *dip;

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
  // 检查 inode 是否有效且持有锁
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
  // 获取 inode 表锁
  acquire(&itable.lock);

  // 检查是否是最后一个引用且 inode 没有链接
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
  uint tot, m;
  struct buf *bp;

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
  uint tot, m;
  struct buf *bp;

  // 检查偏移量是否有效
  if (off > ip->size || off + n < off)
    return -1;
  // 检查是否会超过最大文件大小
  if (off + n > MAXFILE * BSIZE)
    return -1;

  // 循环写入数据，每次处理一个块
  for (tot = 0; tot < n; tot += m, off += m, src += m)
  {
    // 获取包含偏移量的块地址
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
  struct inode *ip;

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
    memmove(name, s, DIRSIZ);
  else
  {
    memmove(name, s, len);
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
  struct inode *ip, *next;

  // 如果路径以 '/' 开头，从根目录开始
  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    // 否则从当前工作目录开始
    ip = idup(myproc()->cwd);

  // 逐个处理路径元素
  while ((path = skipelem(path, name)) != 0)
  {
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
  return ip;
}

// 将路径名转换为 inode
struct inode *
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

// 将路径名转换为父目录 inode 和文件名
struct inode *
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
