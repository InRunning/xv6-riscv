//
// 涉及文件描述符的系统调用的支持函数。
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV]; // devsw(Device Switch 设备分发表) 存放字符设备的读/写回调
struct
{
  struct spinlock lock;    // lock(Spinlock 自旋锁) 保护全局文件表
  struct file file[NFILE]; // file 数组缓存所有内核级文件描述对象
} ftable;

void fileinit(void)
{
  initlock(&ftable.lock, "ftable"); // 初始化全局文件表锁，字符串标识锁名
}

// 分配一个文件结构。
struct file *
filealloc(void)
{
  struct file *f; // f(File) 指向当前检查的文件槽位

  acquire(&ftable.lock); // 进入临界区，遍历全局文件表
  for (f = ftable.file; f < ftable.file + NFILE; f++)
  { // 顺序扫描所有文件槽位
    if (f->ref == 0)
    {                        // 找到引用计数为0的空闲槽位
      f->ref = 1;            // 把引用计数设为1，以占用此文件结构
      release(&ftable.lock); // 离开临界区
      return f;              // 把可用的文件结构返回给调用者
    }
  }
  release(&ftable.lock); // 没找到空闲槽位，也要释放锁
  return 0;              // 返回0表示资源耗尽
}

// 增加文件 f 的引用计数。
struct file *
filedup(struct file *f)
{
  acquire(&ftable.lock); // 修改共享引用计数前先加锁
  if (f->ref < 1)        // 低于1说明出现未初始化或重复释放
    panic("filedup");    // 直接 panic 以暴露 bug
  f->ref++;              // 增加引用计数，表示新的打开者
  release(&ftable.lock); // 解锁
  return f;              // 返回同一个文件指针
}

// 关闭文件 f。（递减引用计数，当计数达到 0 时关闭。）
void fileclose(struct file *f)
{
  struct file ff; // ff(File copy 文件副本) 用于在释放锁后完成清理工作

  acquire(&ftable.lock); // 修改 ref 前进入临界区
  if (f->ref < 1)        // ref 异常说明出现双重关闭
    panic("fileclose");
  if (--f->ref > 0)
  {                        // 递减后仍有引用，直接返回
    release(&ftable.lock); // 离开临界区
    return;                // 其他引用仍在使用，无需真正关闭
  }
  ff = *f;               // 拷贝一份文件结构，稍后在无锁环境下使用
  f->ref = 0;            // 清空引用计数，标记槽位空闲
  f->type = FD_NONE;     // 把类型设为无效，避免悬挂引用
  release(&ftable.lock); // 现在可以离开临界区

  if (ff.type == FD_PIPE)
  {                                  // 管道类型：由 pipe 子系统完成清理
    pipeclose(ff.pipe, ff.writable); // 根据读/写方向做关闭
  }
  else if (ff.type == FD_INODE || ff.type == FD_DEVICE)
  {              // inode 或设备需要文件系统事务
    begin_op();  // 进入日志事务，保证 iput 的修改可持久化
    iput(ff.ip); // 递减 inode 引用计数，必要时回收
    end_op();    // 提交日志事务
  }
}

// 获取文件 f 的元数据。
// addr 是一个用户虚拟地址，指向一个 struct stat 结构体。
int filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc(); // p(Process 进程) 指向当前进程
  struct stat st;            // st(Status 文件状态结构体) 用于暂存元数据

  if (f->type == FD_INODE || f->type == FD_DEVICE)
  {                                                               // 只有 inode/设备才有 stat 信息
    ilock(f->ip);                                                 // 加 inode 睡眠锁，保护元数据
    stati(f->ip, &st);                                            // 把 inode 信息填到 st 临时变量
    iunlock(f->ip);                                               // 释放睡眠锁
    if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0) // 拷贝失败说明用户缓冲区无效
      return -1;
    return 0; // 成功返回 0
  }
  return -1; // 对于管道等类型，无法提供 stat
}

// 从文件 f 读取。
// addr 是一个用户虚拟地址。
// f是文件描述符，从f读取，addr是用户虚拟地址，即要储存读取到的数据的地址，n是读取的字节数
int fileread(struct file *f, uint64 addr, int n)
{
  int r = 0; // r(Result 返回值) 记录实际读取字节数

  if (f->readable == 0) // 文件不允许读，直接报错
    return -1;

  if (f->type == FD_PIPE)
  {                                 // 管道：委托管道层
    r = piperead(f->pipe, addr, n); // 从管道读数据
  }
  else if (f->type == FD_DEVICE)
  {                                                                // 设备文件
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read) // 校验主设备号和回调是否存在
      return -1;
    r = devsw[f->major].read(1, addr, n); // 调用设备读函数，参数 1 代表忽略偏移
  }
  else if (f->type == FD_INODE)
  {                                                 // 普通文件
    ilock(f->ip);                                   // 加 inode 睡眠锁
    if ((r = readi(f->ip, 1, addr, f->off, n)) > 0) // readi 返回读取的字节数
      f->off += r;                                  // 成功读取则推进文件偏移
    iunlock(f->ip);                                 // 解锁 inode
  }
  else
  {
    panic("fileread"); // 其他类型不应该出现
  }

  return r; // 返回读取的字节数或错误码
}

// 向文件 f 写入。
// addr 是一个用户虚拟地址。
int filewrite(struct file *f, uint64 addr, int n)
{
  // 这个例程是每个打开文件对象的单一写入入口点。
  // 它根据 `f->type`（文件类型）分派到不同的实现，
  // 确保操作参与日志（预写日志），并保持文件偏移同步。
  //
  // 关键缩写扩展：
  // - `FD_PIPE`  : 由内存管道支持的文件描述符（先进先出通道）。
  //               例如：pipe() 系统调用创建的管道，父进程通过 fork()
  //               将管道文件描述符传递给子进程，实现进程间通信。
  // - `FD_DEVICE`: 代理字符设备的文件描述符。
  // - `FD_INODE` : 映射到磁盘 inode（索引节点）的文件描述符。
  //
  // 控制流摘要：
  // 1. 如果文件未标记为可写，则拒绝调用。
  // 2. 对于管道和设备，委托给它们专门的写入函数。
  // 3. 对于基于 inode 的文件：
  //    a. 将请求分割成块，这样单个日志事务永远不会
  //       超过 `MAXOPBLOCKS`（最大操作块数）。
  //    b. 用 `begin_op` / `end_op` 包围每个块，这样日志层可以
  //       安全地预留空间和提交。
  //    c. 锁定 inode，调用 `writei`（写入 inode）将字节从用户空间
  //       移动，推进每个描述符的文件偏移，并释放锁。
  // 4. 将任何部分失败转换为 `-1`，否则将字节数冒泡到
  //    调用者。

  int r, ret = 0; // r(Result 单次写入结果)、ret(Return 返回给调用者的总写入字节数)

  if (f->writable == 0) // 不可写则拒绝请求
    return -1;

  if (f->type == FD_PIPE)
  {
    ret = pipewrite(f->pipe, addr, n); // 管道写入直接委托
  }
  else if (f->type == FD_DEVICE)
  {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write) // 检查设备号和写函数
      return -1;
    ret = devsw[f->major].write(1, addr, n); // 调用字符设备写回调
  }
  else if (f->type == FD_INODE)
  {
    // 一次写入几个块以避免超过
    // 最大日志事务大小，包括
    // i-node、间接块、分配块，
    // 以及 2 个用于非对齐写入的冗余块。
    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE; // max(Maximum transaction size 单次日志事务可写字节上限)
    // 公式解释：MAXOPBLOCKS - 1(i-node块) - 1(间接块) - 2(非对齐写入冗余块) = 可用于数据写入的块数
    // 详细说明：
    // 1. i-node块 (第1个1)：存储文件元数据的索引节点，包含文件类型、大小、数据块地址等信息
    //    每个文件操作都需要修改i-node来更新文件大小和时间戳等元数据
    // 2. 间接块 (第2个1)：当文件数据超过12个直接块(NDIRECT)时，需要间接块存储额外数据块的指针
    //    xv6文件系统支持直接块+间接块的混合存储模式，间接块可以指向256个数据块(NINDIRECT=BSIZE/sizeof(uint))
    // 3. 非对齐写入冗余块 (2个)：处理跨块边界的写入操作，确保数据完整性
    //    当写入操作跨越多个块边界时，需要额外的块来处理碎片化写入，避免数据损坏
    // 除以2的原因：每个数据块可能需要对应的间接块支持，实际可用数据块数约为总块数的一半
    int i = 0; // i(Index 索引) 表示已写入偏移
    while (i < n)
    {
      int n1 = n - i; // n1(Chunk length 本轮写入长度)
      if (n1 > max)
        n1 = max;     // 限制单次写入长度不超过日志事务最大限制

      begin_op();                                           // 为每个分块写入开启日志事务
      ilock(f->ip);                                         // 锁住 inode，保证写入原子
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0) // writei 返回写入的字节数
        f->off += r;                                        // 只有成功时才更新文件偏移
      iunlock(f->ip);                                       // 解锁 inode
      end_op();                                             // 提交此次日志事务

      if (r != n1)
      { // 写入长度与期望不一致说明发生错误
        // error from writei
        break; // 跳出循环，稍后返回错误
      }
      i += r; // 正常情况下推进累积写入量
    }
    ret = (i == n ? n : -1); // 如果完成全部写入返回写入字节，否则报错
  }
  else
  {
    panic("filewrite");
  }

  return ret; // 返回写入结果给系统调用层
}
