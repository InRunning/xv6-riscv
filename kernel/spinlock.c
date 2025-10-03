// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void initlock(struct spinlock *lk, char *name) // lk是lock的缩写，name是锁的名称
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

//
// acquire(struct spinlock *lk) - 获取一个自旋锁
//
// @description:
//   循环（“自旋”）等待，直到成功获取指定的锁 `lk`。
//   这个函数是 xv6 中实现互斥访问（mutual exclusion）的关键。
//   在进入一个“临界区”（critical section，即访问共享数据的代码段）之前，
//   必须调用此函数来获取锁，以确保在任何时刻只有一个 CPU 核心可以执行该代码段。
//
// @param *lk:
//   一个指向 `struct spinlock` 的指针，代表希望获取的目标锁。
//
void acquire(struct spinlock *lk) // lk是lock的缩写
{
  // 步骤 1: 关闭中断。这是防止死锁的关键一步。
  // 想象一下：如果一个 CPU 核心获取了锁，然后发生中断，
  // 中断处理程序恰好也想获取同一个锁，那么它就会永远自旋等待，
  // 因为持有锁的核心正忙于处理中断，无法释放锁，从而导致系统死锁。
  // `push_off` 会禁用当前核心的中断，并在一个计数器中记录下来。
  push_off();

  // 步骤 2: 检查当前 CPU 是否已经持有该锁。
  // 自旋锁是不可重入的（non-reentrant）。如果一个核心已经持有了某个锁，
  // 它不能再次尝试获取同一个锁，否则会立即导致死锁（自己等待自己释放）。
  // `holding()` 函数用于进行此项检查。
  if (holding(lk))
    panic("acquire"); // 如果发生，系统崩溃并报告错误。

  // 步骤 3: 原子地尝试获取锁（核心操作）。
  // `__sync_lock_test_and_set` 是 GCC/Clang 的一个内置原子操作函数。
  // 在 RISC-V 上，它会被编译成一条 `amoswap.w.aq` (Atomic Memory Operation: Swap) 指令。
  // 这条指令会原子地完成以下两件事：
  //   1. 将 `lk->locked` 的当前值加载到一个寄存器中。
  //   2. 将新值 `1` 写入到 `lk->locked`。
  // 整个操作是不可中断的，保证了原子性。
  //
  // 循环条件 `!= 0` 的含义是：
  // - 如果 `lk->locked` 原来是 0（未锁定），`amoswap` 会返回 0，并将 `lk->locked` 置为 1。
  //   此时循环条件为 false，循环结束，锁获取成功。
  // - 如果 `lk->locked` 原来是 1（已锁定），`amoswap` 会返回 1，并将 `lk->locked` 再次置为 1。
  //   此时循环条件为 true，CPU 会继续在这个 `while` 循环中"自旋"，不断重试，直到持有锁的 CPU 释放它。
  while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ; // 空循环体，CPU 在这里忙等待（busy-waiting）。

  // 步骤 4: 设置内存屏障（Memory Barrier/Fence）。
  // `__sync_synchronize` 会被编译成一条 `fence` 指令。
  // 这条指令告诉 CPU 和编译器：禁止将此 `fence` 指令之后的任何读写内存操作，
  // 重新排序到 `fence` 之前。
  // 这确保了所有在临界区内对共享内存的访问，都严格发生在我们成功获取锁之后。
  __sync_synchronize();

  // 步骤 5: 记录锁的持有者信息。
  // 当锁成功获取后，我们将当前 CPU 的指针保存到锁的 `cpu` 字段中。
  // 这主要用于调试和 `holding()` 函数的判断。
  lk->cpu = mycpu();
}

// Release the lock.
void release(struct spinlock *lk) // lk是lock的缩写
{
  if (!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
}

//
// holding(struct spinlock *lk) - 检查当前 CPU 是否持有指定的锁
//
// @description:
//   用于断言和防止死锁。它必须在中断被关闭的情况下调用。
//
// @return:
//   如果当前 CPU 正持有锁 `lk`，返回 1 (true)；否则返回 0 (false)。
//
int holding(struct spinlock *lk) // lk是lock的缩写
{
  int r; // r是result的缩写，返回结果
  // 检查两个条件：
  // 1. `lk->locked` 是否为 1 (锁是否确实被占用了)。
  // 2. `lk->cpu` 是否指向当前 CPU 的结构体 (占用者是不是我自己)。
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

//
// push_off(void) / pop_off(void) - 可嵌套的中断禁用/启用机制
//
// @description:
//   `push_off` 和 `pop_off` 是一对用于管理中断状态的函数。与简单的 `intr_off()` 不同，
//   它们是可嵌套的。例如，调用两次 `push_off` 需要调用两次 `pop_off` 才能最终恢复中断
//   （仅当最外层的 `push_off` 调用前中断是开启状态时）。
//   这在有嵌套临界区的情况下非常有用。
//
void push_off(void)
{
  // 1. 保存当前的中断状态（是开还是关）。intr是interrupt的缩写。
  int old = intr_get(); // old是旧的（原始）中断状态

  // 2. 立即关闭当前 CPU 核心的中断。
  intr_off();

  // 3. 记录原始中断状态并增加嵌套层数。
  // `mycpu()` 返回一个指向当前 CPU 状态结构体的指针。
  // `noff` 是一个计数器，记录了 `push_off` 被调用的次数。noff是nested off的缩写，即嵌套层数
  if (mycpu()->noff == 0)
    // 如果这是第一次调用 `push_off` (嵌套层数为0)，
    // 就把原始的中断状态 `old` 保存到 `intena` 字段中。intena是interrupt enable的缩写，即中断启用状态
    mycpu()->intena = old;
  // 嵌套层数加一。
  mycpu()->noff += 1;
}

void pop_off(void)
{
  struct cpu *c = mycpu(); // c是cpu的缩写，指向当前CPU结构体
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}