#include "types.h"          // 基础整数类型定义
#include "riscv.h"          // RISC-V 架构相关常量与函数
#include "defs.h"           // 内核公共函数声明
#include "param.h"          // 系统参数配置
#include "spinlock.h"       // 旋转锁(Spin Lock)接口
#include "proc.h"           // 进程(Process)结构与函数
#include "fs.h"             // 文件系统(File System)接口
#include "sleeplock.h"      // 睡眠锁(Sleep Lock)接口
#include "file.h"           // 文件(File)抽象

#define PIPESIZE 512        // 管道缓存区大小为512字节

struct pipe {
  struct spinlock lock;     // lock(Spin Lock 旋转锁)用于保护管道共享数据
  char data[PIPESIZE];      // data 管道环形缓冲区
  uint nread;               // nread(Number Read 已读取字节数)
  uint nwrite;              // nwrite(Number Write 已写入字节数)
  int readopen;             // readopen 标记读端是否仍被打开
  int writeopen;            // writeopen 标记写端是否仍被打开
};

int
pipealloc(struct file **f0, struct file **f1)  // 为管道分配文件对象并初始化
{
  struct pipe *pi;          // pi(Pipe 管道结构指针)

  pi = 0;                   // 初始设为NULL指针
  *f0 = *f1 = 0;            // 将两个文件指针初始化为空
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)  // 分配两端文件结构
    goto bad;               // 任一失败则跳转到清理逻辑
  if((pi = (struct pipe*)kalloc()) == 0)       // kalloc(Kernel Allocate 内核内存分配)申请管道内存
    goto bad;               // 分配失败同样跳转清理
  pi->readopen = 1;         // 读端初始打开
  pi->writeopen = 1;        // 写端初始打开
  pi->nwrite = 0;           // 已写入字节计数清零
  pi->nread = 0;            // 已读取字节计数清零
  initlock(&pi->lock, "pipe");  // 初始化自旋锁并命名为"pipe"
  (*f0)->type = FD_PIPE;    // type 设置为管道文件类型
  (*f0)->readable = 1;      // readable 可读
  (*f0)->writable = 0;      // writable 只读
  (*f0)->pipe = pi;         // 将管道结构挂接到文件
  (*f1)->type = FD_PIPE;    // 第二个文件同样设置为管道类型
  (*f1)->readable = 0;      // readable 只写
  (*f1)->writable = 1;      // writable 可写
  (*f1)->pipe = pi;         // 共享同一个管道结构
  return 0;                 // 分配成功返回0

 bad:                        // 清理标签
  if(pi)                     // 若管道结构已分配
    kfree((char*)pi);        // kfree(Kernel Free 内核释放)释放内存
  if(*f0)                    // 若第一端文件已分配
    fileclose(*f0);          // fileclose 关闭文件并释放
  if(*f1)                    // 若第二端文件已分配
    fileclose(*f1);          // 关闭第二个文件
  return -1;                 // 返回错误
}

void
pipeclose(struct pipe *pi, int writable)  // 关闭指定方向的管道并在必要时释放
{
  acquire(&pi->lock);        // acquire 获取自旋锁进入临界区
  if(writable){              // 如果关闭的是写端
    pi->writeopen = 0;       // 标记写端关闭
    wakeup(&pi->nread);      // wakeup 唤醒可能等待读取的进程
  } else {                   // 否则关闭读端
    pi->readopen = 0;        // 标记读端关闭
    wakeup(&pi->nwrite);     // 唤醒可能等待写入的进程
  }
  if(pi->readopen == 0 && pi->writeopen == 0){ // 当读写都关闭
    release(&pi->lock);      // release 释放自旋锁
    kfree((char*)pi);        // 释放管道结构内存
  } else
    release(&pi->lock);      // 至少一端仍然打开，仅释放锁
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)  // 从用户缓冲区写入管道
{
  int i = 0;                 // i 当前已写入字节数
  struct proc *pr = myproc();  // pr(Process 进程指针)为当前进程

  acquire(&pi->lock);        // 获取管道锁以保护共享数据
  while(i < n){              // 循环直至写满请求字节
    if(pi->readopen == 0 || killed(pr)){  // killed 检查进程是否被标记为结束
      release(&pi->lock);    // 管道读端关闭或进程死亡则退出
      return -1;             // 返回错误
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ // 管道缓冲区已满 //DOC: pipewrite-full
      wakeup(&pi->nread);    // 唤醒等待读取的进程
      sleep(&pi->nwrite, &pi->lock); // sleep 让出CPU等待空间
    } else {
      char ch;               // ch 单个字节临时缓冲
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1)  // copyin 将用户数据复制到内核
        break;               // 复制失败则停止
      pi->data[pi->nwrite++ % PIPESIZE] = ch;  // 写入环形缓冲区并递增写指针
      i++;                   // 成功写入一个字节
    }
  }
  wakeup(&pi->nread);        // 唤醒等待读取的进程
  release(&pi->lock);        // 释放管道锁

  return i;                  // 返回实际写入字节数
}

int
piperead(struct pipe *pi, uint64 addr, int n)  // 将管道数据读出到用户空间
{
  int i;                     // i 已读取字节数
  struct proc *pr = myproc();  // pr(Process 进程指针)为当前进程
  char ch;                   // ch 单字节临时缓冲

  acquire(&pi->lock);        // 获取管道锁
  while(pi->nread == pi->nwrite && pi->writeopen){  // 缓冲区为空且写端仍打开 //DOC: pipe-empty
    if(killed(pr)){          // 如果当前进程被杀死
      release(&pi->lock);    // 释放锁并退出
      return -1;             // 返回错误
    }
    sleep(&pi->nread, &pi->lock); // 睡眠等待写端写入 //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){    // 逐字节读取 //DOC: piperead-copy
    if(pi->nread == pi->nwrite)  // 数据读取完毕
      break;                 // 跳出循环
    ch = pi->data[pi->nread++ % PIPESIZE];  // 从环形缓冲区取出一个字节
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1)  // copyout 将数据传回用户空间
      break;                 // 复制失败则停止
  }
  wakeup(&pi->nwrite);       // 唤醒可能等待空间的写进程 //DOC: piperead-wakeup
  release(&pi->lock);        // 释放管道锁
  return i;                  // 返回实际读取字节数
}
