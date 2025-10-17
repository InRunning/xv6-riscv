#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// 简单的日志系统，允许并发的文件系统系统调用。
//
// 一个日志事务包含多个文件系统系统调用的更新。日志系统只在
// 没有活跃的文件系统系统调用时才提交。因此永远不需要考虑
// 提交是否会写入未提交系统调用的更新到磁盘。
//
// 系统调用应该调用begin_op()/end_op()来标记其开始和结束。
// 通常begin_op()只是增加进行中的文件系统系统调用的计数并返回。
// 但如果它认为日志即将用尽，它会休眠直到最后一个未完成的end_op()提交。
//
// 日志是一个包含磁盘块的物理重做日志。
// 磁盘上的日志格式：
//   头部块，包含块A、B、C等的块号
//   块A
//   块B
//   块C
//   ...
// 日志追加是同步的。

// 头部块的内容，用于磁盘上的头部块和在提交前在内存中跟踪已记录的块号
struct logheader
{
  int n;                // 日志中记录的块数量
  int block[LOGBLOCKS]; // 日志中记录的块号数组
};

// 日志结构体，维护日志系统的状态
struct log
{
  struct spinlock lock; // 保护日志结构的自旋锁
  int start;            // 日志在磁盘上的起始块号
  int outstanding;      // 当前正在执行的文件系统系统调用数量
  int committing;       // 是否正在提交中，如果是则等待
  int dev;              // 日志所在的设备号
  struct logheader lh;  // 内存中的日志头部
};
struct log log; // 全局日志实例

static void recover_from_log(void);
static void commit();

// 初始化日志系统
// 参数:
//   dev - 设备号，指定日志存储在哪个磁盘设备上
//   sb - 超级块指针，包含文件系统的元数据，包括日志的起始位置
void initlog(int dev, struct superblock *sb)
{
  // 检查日志头部结构体的大小是否超过一个磁盘块的大小
  // 虽然sizeof(struct logheader)和BSIZE都是编译时常量，但这个检查仍然很重要
  // 这是一种防御性编程，确保在代码修改或配置变更时（如LOGBLOCKS数量增加）
  // 日志头部仍然能完整地存储在一个磁盘块中，否则会导致日志系统无法正常工作
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  // 初始化日志系统的自旋锁，用于保护日志数据结构的并发访问
  // 锁的名称为"log"，便于调试时识别
  initlock(&log.lock, "log");

  // 设置日志在磁盘上的起始块号
  // 这个值来自超级块中的logstart字段，指示日志区域的起始位置
  log.start = sb->logstart;

  // 记录日志所在的设备号，用于后续的磁盘读写操作
  log.dev = dev;

  // 从日志中恢复数据
  // 在系统启动时调用，检查是否有未完成的事务需要恢复
  // 如果系统在事务提交过程中崩溃，这个函数会确保文件系统的一致性
  recover_from_log();
}

// 将已提交的块从日志复制到它们的原始位置
// 参数:
//   recovering - 是否处于恢复模式，1表示恢复模式，0表示正常提交
static void
install_trans(int recovering)
{
  int tail;

  // 遍历日志中的所有块
  for (tail = 0; tail < log.lh.n; tail++)
  {
    // 如果是恢复模式，打印恢复信息
    if (recovering)
    {
      printf("recovering tail %d dst %d\n", tail, log.lh.block[tail]);
    }
    // 读取日志块
    struct buf *lbuf = bread(log.dev, log.start + tail + 1);
    // 读取目标块（原始位置）
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]);
    // 将日志块的数据复制到目标块
    memmove(dbuf->data, lbuf->data, BSIZE);
    // 将目标块写入磁盘
    bwrite(dbuf);
    // 如果不是恢复模式，取消固定目标块（允许被换出）
    if (recovering == 0)
      bunpin(dbuf);
    // 释放日志块和目标块的缓冲区
    brelse(lbuf);
    brelse(dbuf);
  }
}

// 从磁盘读取日志头部到内存中的日志头部
static void
read_head(void)
{
  // 读取日志头部块
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *)(buf->data);
  int i;
  // 复制日志块数量
  log.lh.n = lh->n;
  // 复制所有日志块号
  for (i = 0; i < log.lh.n; i++)
  {
    log.lh.block[i] = lh->block[i];
  }
  // 释放缓冲区
  brelse(buf);
}

// 将内存中的日志头部写入磁盘
// 这是当前事务真正提交的点
static void
write_head(void)
{
  // 读取日志头部块
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *)(buf->data);
  int i;
  // 复制日志块数量
  hb->n = log.lh.n;
  // 复制所有日志块号
  for (i = 0; i < log.lh.n; i++)
  {
    hb->block[i] = log.lh.block[i];
  }
  // 写入磁盘
  bwrite(buf);
  // 释放缓冲区
  brelse(buf);
}

// 从日志中恢复数据
// 在系统启动时调用，检查是否有未完成的事务需要恢复
static void
recover_from_log(void)
{
  // 读取日志头部
  read_head();
  // 如果有已提交的事务，将其从日志复制到磁盘
  install_trans(1);
  // 清空内存中的日志头部
  log.lh.n = 0;
  // 清空磁盘上的日志
  write_head();
}

// 在每个文件系统系统调用开始时调用
// 标记一个事务的开始，并确保有足够的日志空间
void begin_op(void)
{
  // 获取日志锁
  acquire(&log.lock);
  while (1)
  {
    // 如果正在提交中，休眠等待
    if (log.committing)
    {
      sleep(&log, &log.lock);
    }
    else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGBLOCKS)
    {
      // 检查当前操作是否会耗尽日志空间
      // log.lh.n: 当前已记录在日志中的块数量（已提交但未写入磁盘的修改）
      // log.outstanding: 当前正在进行的文件系统系统调用数量
      // (log.outstanding + 1): 包括当前正在开始的这个新操作
      // MAXOPBLOCKS: 每个文件系统系统调用最多可能使用的块数
      // LOGBLOCKS: 日志总共能容纳的块数（日志容量限制）
      //
      // 这个条件判断：当前已用空间 + 新操作预计需要的空间 > 总容量
      // 如果为真，说明日志空间即将耗尽，需要等待现有操作提交完成以释放空间
      sleep(&log, &log.lock);
    }
    else
    {
      // 增加进行中的系统调用计数
      log.outstanding += 1;
      // 释放锁并返回
      release(&log.lock);
      break;
    }
  }
}

// 在每个文件系统系统调用结束时调用
// 如果这是最后一个未完成的操作，则提交事务，end_op() 执行时 log.outstanding 已经减 1，相当于释放出一部分日志容量：即使当前还不是最后一个操作要提交，新的空间也可能已经足够让某个等待者继续执行。所以这里要 wakeup(&log);
void end_op(void)
{
  int do_commit = 0;

  // 获取日志锁
  acquire(&log.lock);
  // 减少进行中的系统调用计数
  log.outstanding -= 1;
  // 检查是否正在提交中，如果是则报错
  if (log.committing)
    panic("log.committing");
  // 如果没有进行中的系统调用，准备提交
  if (log.outstanding == 0)
  {
    do_commit = 1;
    log.committing = 1;
  }
  else
  {
    // begin_op()可能在等待日志空间，
    // 减少log.outstanding已经减少了保留的空间量
    // 唤醒等待的进程（sleep() 同样以 &log 作为 channel 进行匹配）
    wakeup(&log);
  }
  // 释放锁
  release(&log.lock);

  // 如果需要提交
  if (do_commit)
  {
    // 在不持有锁的情况下调用commit，因为不允许在持有锁时休眠
    commit();
    // 重新获取锁
    acquire(&log.lock);
    // 提交完成，重置标志
    log.committing = 0;
    // 唤醒可能等待的进程（同一 channel：&log）
    wakeup(&log);
    // 释放锁
    release(&log.lock);
  }
}

// 将修改过的块从缓存复制到日志
static void
write_log(void)
{
  int tail;

  // 遍历所有需要记录的块
  for (tail = 0; tail < log.lh.n; tail++)
  {
    // 读取日志块（目标位置）
    struct buf *to = bread(log.dev, log.start + tail + 1);
    // 读取缓存块（源位置）
    struct buf *from = bread(log.dev, log.lh.block[tail]);
    // 将缓存块的数据复制到日志块
    memmove(to->data, from->data, BSIZE);
    // 将日志块写入磁盘
    bwrite(to);
    // 释放源块和目标块的缓冲区
    brelse(from);
    brelse(to);
  }
}

// 提交当前事务
// 将所有修改的块从缓存写入日志，然后从日志写入到它们的原始位置
static void
commit()
{
  // 如果有需要提交的块
  if (log.lh.n > 0)
  {
    write_log();      // 将修改过的块从缓存写入日志
    write_head();     // 将头部写入磁盘 -- 真正的提交点
    install_trans(0); // 现在将写入安装到原始位置
    // 清空内存中的日志头部
    log.lh.n = 0;
    write_head(); // 从日志中擦除事务
  }
}

// 调用者已经修改了b->data并且完成了对缓冲区的操作
// 记录块号并通过增加引用计数来固定缓存中的块
// commit()/write_log()将执行实际的磁盘写入
//
// log_write() 替代了 bwrite()；典型用法是：
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
//
// 术语说明：
// - `log.lh`: 日志头结构，跟踪每个排队等待提交的脏块号（`lh` 代表日志头）。
// - `bpin`: 缓冲区固定（防止回收）的辅助函数，用于增加缓冲区缓存内的引用计数。
// - `log.outstanding`: 活动文件系统系统调用的数量，这样日志层可以检测到当最终写入者退出其临界区时。
//
// 控制流程：
// 1. 获取全局日志锁，以序列化对内存头部的更新。
// 2. 拒绝超出 `LOGBLOCKS`（日志可以容纳的最大块数）的入队尝试，或在事务外发生的写入。
// 3. 吸收重复的块号，这样单个磁盘块在事务中只出现一次——后来的修改只是重用较早的槽位。
// 4. 固定新添加的缓冲区，这样在提交序列完成之前，缓冲区缓存无法回收它们。
// 5. 释放锁；调用者现在可以用 `brelse`（缓冲区释放）来丢弃其缓冲区引用。
void log_write(struct buf *b)
{
  int i;

  // 获取日志锁
  acquire(&log.lock);
  // 检查事务是否过大
  // 检查事务是否过大（Transaction Size Check）
  // log.lh.n: 当前日志中记录的块数量
  // LOGBLOCKS: 日志系统支持的最大块数（日志容量限制）
  // 如果当前事务的块数超过日志容量限制，则触发 panic
  if (log.lh.n >= LOGBLOCKS)
    panic("too big a transaction");
  // 检查是否在事务外调用（Transaction Boundary Check）
  // log.outstanding: 当前正在执行的文件系统系统调用数量（outstanding calls）
  // 如果 outstanding < 1，表示没有活跃的事务，此时调用 log_write 是错误的
  // 这确保了所有文件系统修改都必须在 begin_op() 和 end_op() 之间进行，都是防御性编程
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  // 检查块是否已经在日志中（日志吸收）
  for (i = 0; i < log.lh.n; i++)
  {
    if (log.lh.block[i] == b->blockno)
      break;
  }
  // 记录块号
  log.lh.block[i] = b->blockno;
  // 如果是新块，添加到日志中
  if (i == log.lh.n)
  {
    // 固定缓存中的块，防止被换出
    bpin(b);
    // 增加日志中的块数量
    log.lh.n++;
  }
  // 释放锁
  release(&log.lock);
}
