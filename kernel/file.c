//
// Support functions for system calls that involve file descriptors.
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

// Allocate a file structure.
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

// Increment ref count for file f.
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

// Close file f.  (Decrement ref count, close when reaches 0.)
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

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
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

// Read from file f.
// addr is a user virtual address.
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

// Write to file f.
// addr is a user virtual address.
int filewrite(struct file *f, uint64 addr, int n)
{
  // This routine is the single write entry point for every open file object.
  // It fans out to different implementations based on `f->type` (file type),
  // ensures the operation participates in the log (write-ahead logging), and
  // keeps the file offset in sync.
  //
  // Key abbreviations expanded:
  // - `FD_PIPE`  : file descriptor backed by an in-memory pipe (First-In First-Out channel).
  // - `FD_DEVICE`: file descriptor that proxies a character device.
  // - `FD_INODE` : file descriptor that maps to an on-disk inode (index node).
  //
  // Control flow summary:
  // 1. Reject the call if the file is not marked writable.
  // 2. For pipes and devices, delegate to their specialised writer functions.
  // 3. For inode-backed files:
  //    a. Split the request into chunks so a single log transaction never
  //       exceeds `MAXOPBLOCKS` (maximum operation blocks).
  //    b. Surround each chunk with `begin_op` / `end_op` so the log layer can
  //       reserve space and commit safely.
  //    c. Lock the inode, call `writei` (write inode) to move bytes from user
  //       space, advance the per-descriptor file offset, and release the lock.
  // 4. Convert any partial failure into `-1`, otherwise bubble the byte count to
  //    the caller.

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
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE; // max(Maximum transaction size 单次日志事务可写字节上限)
    int i = 0;                                         // i(Index 索引) 表示已写入偏移
    while (i < n)
    {
      int n1 = n - i; // n1(Chunk length 本轮写入长度)
      if (n1 > max)
        n1 = max;

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
