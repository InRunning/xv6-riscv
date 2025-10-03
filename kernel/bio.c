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
struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // 所有缓冲区的链表，通过 prev/next 连接。
  // 按最近使用的顺序排序。
  // head.next 是最近使用的，head.prev 是最久未使用的。
  struct buf head;
} bcache;

// 3. 函数 ----

// 3.1. 初始化 ----
// 初始化缓冲区缓存
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // 创建缓冲区链表
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// 3.2. 缓冲区管理 ----

// 在缓冲区缓存中查找指定设备和块号的块。
// 如果未找到，则分配一个缓冲区。
// 无论哪种情况，都返回一个锁定的缓冲区。
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // 该块是否已经被缓存？
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 未缓存。
  // 回收最近最少使用（LRU）的未使用缓冲区。
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
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
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// 将缓冲区 b 的内容写入磁盘。必须持有锁。
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// 释放一个锁定的缓冲区。
// 将其移动到最近使用列表的头部。
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
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
void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

// 减少缓冲区的引用计数。
void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}