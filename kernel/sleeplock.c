// 睡眠锁
//
// 睡眠锁是一种同步机制，它允许持有锁的进程在等待资源时进入睡眠状态。
// 当一个进程尝试获取一个已经被持有的睡眠锁时，它会释放其持有的自旋锁，
// 然后进入睡眠状态，直到锁被释放并被唤醒。
// 这种机制避免了忙等待，从而提高了 CPU 的利用率。

// 1. 头文件 ----
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

// 2. 函数 ----

// 2.1. 初始化 ----
// initsleeplock 函数：初始化一个睡眠锁。
// 参数:
//   lk: 指向要初始化的 sleeplock 结构的指针。
//   name: 锁的名称，用于调试。
void
initsleeplock(struct sleeplock *lk, char *name)
{
  // 初始化睡眠锁内部的自旋锁，用于保护睡眠锁的状态变量（locked, pid）。
  initlock(&lk->lk, "sleep lock");
  lk->name = name;    // 设置锁的名称
  lk->locked = 0;     // 初始状态为未锁定
  lk->pid = 0;        // 初始时没有进程持有锁
}

// 2.2. 获取锁 ----
// acquiresleep 函数：获取一个睡眠锁。
// 如果锁已被其他进程持有，则当前进程会进入睡眠状态，直到锁被释放。
// 参数:
//   lk: 指向要获取的 sleeplock 结构的指针。
void
acquiresleep(struct sleeplock *lk)
{
  // 首先获取睡眠锁内部的自旋锁，以保护对睡眠锁状态的访问。
  acquire(&lk->lk);
  // 如果锁已经被持有，则当前进程进入睡眠状态。
  // sleep 函数会释放 lk->lk（自旋锁），并等待 lk 上的唤醒事件。
  // 当被唤醒时，sleep 函数会重新获取 lk->lk。
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  // 锁已被获取，更新锁的状态。
  lk->locked = 1;             // 标记锁为已锁定
  lk->pid = myproc()->pid;    // 记录持有锁的进程 ID
  // 释放睡眠锁内部的自旋锁。
  release(&lk->lk);
}

// 2.3. 释放锁 ----
// releasesleep 函数：释放一个睡眠锁。
// 释放锁后，会唤醒所有在该锁上等待的进程。
// 参数:
//   lk: 指向要释放的 sleeplock 结构的指针。
void
releasesleep(struct sleeplock *lk)
{
  // 获取睡眠锁内部的自旋锁，以保护对睡眠锁状态的访问。
  acquire(&lk->lk);
  // 释放锁，更新锁的状态。
  lk->locked = 0;     // 标记锁为未锁定
  lk->pid = 0;        // 清除持有锁的进程 ID
  // 唤醒所有在该锁上等待的进程。
  wakeup(lk);
  // 释放睡眠锁内部的自旋锁。
  release(&lk->lk);
}

// 2.4. 检查锁状态 ----
// holdingsleep 函数：检查当前进程是否持有指定的睡眠锁。
// 返回值:
//   如果当前进程持有该锁，则返回 1；否则返回 0。
// 参数:
//   lk: 指向要检查的 sleeplock 结构的指针。
int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  // 获取睡眠锁内部的自旋锁，以保护对睡眠锁状态的访问。
  acquire(&lk->lk);
  // 检查锁是否被锁定，并且持有锁的进程 ID 是否与当前进程的 ID 相同。
  r = lk->locked && (lk->pid == myproc()->pid);
  // 释放睡眠锁内部的自旋锁。
  release(&lk->lk);
  return r;
}