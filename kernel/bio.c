// 缓冲区缓存。
//
// 缓冲区缓存是一个 `buf` 结构的链表，其中包含磁盘块内容的缓存副本。
// 在内存中缓存磁盘块可以减少磁盘读取的次数，并为多个进程使用的磁盘块提供一个同步点。
//
// 接口说明:
// * 要获取特定磁盘块的缓冲区，请调用 bread。
// * 更改缓冲区数据后，调用 bwrite 将其写入磁盘。
// * 使用完缓冲区后，调用 brelse。
// * 调用 brelse 后，请勿再使用该缓冲区。
// * 同一时间只有一个进程可以使用一个缓冲区，因此不要占用超过必要的时间。

// 1. 头文件 ----
#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// 2. 全局变量 ----
struct
{
  struct spinlock lock;
  struct buf buf[NBUF];

  // 所有缓冲区的链表，通过 prev/next 连接。
  // 按最近使用的顺序排序。
  // head.next 是最近使用的，head.prev 是最久未使用的。
  struct buf head;
} bcache;

// 3. 函数 ----

// 3.1. 初始化 ----
// 3.1. 初始化 ----
// binit 函数：初始化缓冲区缓存。
// 该函数在系统启动时调用一次，用于设置缓冲区缓存的数据结构。
void binit(void)
{
  struct buf *b;

  // 初始化 bcache 的自旋锁，用于保护整个缓冲区缓存的元数据（如链表结构）。
  initlock(&bcache.lock, "bcache");

  // 创建一个循环双向链表来管理所有缓冲区。
  // bcache.head 是一个哨兵节点，它不存储实际的磁盘数据，
  // 而是作为链表的头尾，简化了链表操作的边界条件。
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;

  // 遍历所有的缓冲区 `bcache.buf[NBUF]`，将它们添加到链表中。
  // 初始时，所有缓冲区都按顺序添加到链表的头部，
  // 使得 `bcache.head.next` 指向最近添加的缓冲区，
  // `bcache.head.prev` 指向最先添加的缓冲区（即最久未使用的）。
  // NBUF (Number of Buffers) 定义了磁盘块缓存中缓冲区的数量，目前是一个常量30
  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    // 将当前缓冲区 b 插入到链表的头部，使其成为新的最最近使用的缓冲区。
    // 具体操作是将 b 放在 bcache.head 和原来的 bcache.head.next 之间。
    b->next = bcache.head.next; // b 的 next 指向当前链表的第一个元素
    b->prev = &bcache.head;     // b 的 prev 指向链表头（哨兵节点）
    // 初始化每个缓冲区的睡眠锁，用于保护缓冲区的数据内容。
    // 当一个进程正在读写某个缓冲区时，其他进程需要等待该锁。
    initsleeplock(&b->lock, "buffer");
    // 更新链表中相邻节点的指针，完成双向链表的插入。
    bcache.head.next->prev = b; // 原来的第一个元素的 prev 现在指向 b
    bcache.head.next = b;       // 链表头的 next 现在指向 b，使 b 成为新的第一个元素
  }
}
// 3.2. 缓冲区管理 ----

// 在缓冲区缓存中查找指定设备和块号的块。
// 如果未找到，则分配一个缓冲区。
// 无论哪种情况，都返回一个锁定的缓冲区。
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // 该块是否已经被缓存？
  for (b = bcache.head.next; b != &bcache.head; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 未缓存。
  // 回收最近最少使用（LRU）的未使用缓冲区。
  for (b = bcache.head.prev; b != &bcache.head; b = b->prev)
  {
    if (b->refcnt == 0)
    {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// 返回一个包含指定块内容的锁定缓冲区。
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// 将缓冲区 b 的内容写入磁盘。必须持有锁。
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// 释放一个锁定的缓冲区。
// 将其移动到最近使用列表的头部。
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0)
  {
    // 没有进程在等待它。
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }

  release(&bcache.lock);
}

// 3.3. 引用计数 ----

// 增加缓冲区的引用计数，防止其被回收。
void bpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

// 减少缓冲区的引用计数。
void bunpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}