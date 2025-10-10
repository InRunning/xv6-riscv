// ====================================================================
// 1. INCLUDES
// ====================================================================
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// ====================================================================
// 2. GLOBAL VARIABLES & FORWARD DECLARATIONS
// ====================================================================
struct cpu cpus[NCPU];   // CPU状态数组
struct proc proc[NPROC]; // 进程表数组
struct proc *initproc;   // 指向第一个用户进程 (init)

int nextpid = 1;          // 下一个可用的进程ID
struct spinlock pid_lock; // 保护 nextpid 的自旋锁

extern void forkret(void);            // 在 forkret.S 中定义，是 fork 子��程的返回点
static void freeproc(struct proc *p); // 释放一个进程结构及其资源的函数声明

extern char trampoline[]; // 在 trampoline.S 中定义，指向陷阱处理代码

// 保护父子进程关系（p->parent）的锁。
// 确保 wait() 的父进程不会丢失子进程的唤醒信号。
// 在获取任何 p->lock 之前必须先获取此锁。
struct spinlock wait_lock;

// ====================================================================
// 3. FUNCTIONS
// ====================================================================

// --------------------------------------------------------------------
// proc_mapstacks
// --------------------------------------------------------------------
// 为每个进程的内核栈分配一个页面。
// 将其映射到内核地址空间的高位地址，后面紧跟着一个无效的保护页面。
// @param kpgtbl: 内核页表
// --------------------------------------------------------------------
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    // 分配一页物理内存作为内核栈
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");

    // 计算当前进程的内核栈的虚拟地址
    uint64 va = KSTACK((int)(p - proc));

    // 将物理页映射到虚拟地址
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// --------------------------------------------------------------------
// procinit
// --------------------------------------------------------------------
// 初始化进程表和相关锁。
// 在系统启动时由 main() 调用一次。
// --------------------------------------------------------------------
void procinit(void)
{
  struct proc *p;

  // 初始化用于分配PID和用于wait()的锁
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  // 遍历进程表，初始化每个进程槽
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");          // 初始化每个进程自己的锁
    p->state = UNUSED;                   // 将状态标记为未使用
    p->kstack = KSTACK((int)(p - proc)); // 设置内核栈的虚拟地址
  }
}

// --------------------------------------------------------------------
// cpuid
// --------------------------------------------------------------------
// 获取当前CPU的ID（hart id）。
// @return: 当前CPU的ID。
// @note: 必须在禁用中断的情况下调用，以防止进程被迁移到其他CPU导致竞争。
// --------------------------------------------------------------------
int cpuid()
{
  // RISC-V中，`tp` 寄存器通常用于存储 hart id。
  int id = r_tp();
  return id;
}

// --------------------------------------------------------------------
// mycpu
// --------------------------------------------------------------------
// 获取当前CPU的 `struct cpu` 结构体指针。
// @return: 指向当前CPU结构体的指针。
// @note: 必须禁用中断。
// --------------------------------------------------------------------
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// --------------------------------------------------------------------
// myproc
// --------------------------------------------------------------------
// 获取当前CPU上正在运行的进程的 `struct proc` 结构体指针。
// @return: 指向当前进程结构体的指针，如果没有进程在运行则为0。
// --------------------------------------------------------------------
struct proc *
myproc(void)
{
  // 禁用中断，以确保 `mycpu()` 和 `c->proc` 的读取是原子的。
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  // 恢复之前的中断状态。
  pop_off();
  return p;
}

// --------------------------------------------------------------------
// allocpid
// --------------------------------------------------------------------
// 分配一个唯一的进程ID (PID)。
// @return: 新的PID。
// --------------------------------------------------------------------
int allocpid()
{
  int pid;

  // 加锁以保护对 `nextpid` 的访问
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// --------------------------------------------------------------------
// allocproc
// --------------------------------------------------------------------
// 在进程表中查找一个未使用的进程槽（`UNUSED`状态），并进行初始化。
// 如果成功，它会初始化进程的基本内核状态，并返回时持有该进程的锁 `p->lock`。
// @return: 成功则返回指向新分配的 `struct proc` 的指针，失败则返回0。
// --------------------------------------------------------------------
static struct proc *
allocproc(void)
{
  struct proc *p;

  // 遍历进程表，寻找一个未使用的进程
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found; // 找到后跳转到初始化代码
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0; // 没有找到空闲进程

found:
  p->pid = allocpid(); // 分配PID
  p->state = USED;     // 设置状态为USED

  // 为进程分配一个陷阱帧（trapframe）页面，陷阱帧用于存储进程从用户态切换到内核态时的用户寄存器的值
  // 陷阱帧是 xv6 内核中一个非常重要的数据结构，它在进程处理系统调用、中断和异常时起到关键作用：

  // 定义和位置：

  // 陷阱帧结构体定义在 kernel/proc.h:56-93
  // 每个进程都有一个陷阱帧，存储在一个单独的物理页面中
  // 陷阱帧位于用户页表中，紧邻 trampoline 页面之下
  // 主要功能：

  // 保存用户态寄存器：当进程从用户态切换到内核态时，所有用户寄存器的值被保存到陷阱帧中
  // 恢复用户态执行：当从内核态返回用户态时，从陷阱帧中恢复用户寄存器的值
  // 系统调用参数传递：系统调用的参数和返回值通过陷阱帧中的寄存器传递
  // 进程状态维护：保存用户程序计数器（epc）等关键状态信息
  // 工作流程：

  // 用户进程执行系统调用或发生中断/异常
  // 硬件切换到内核态，跳转到 trampoline 代码
  // trampoline 代码将用户寄存器保存到陷阱帧
  // 调用内核处理函数（如 usertrap()）
  // 处理完成后，从陷阱帧恢复寄存器，返回用户态
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p); // 分配失败，释放已占用的资源
    release(&p->lock);
    return 0;
  }

  // 为进程创建一个用户页表（初始为空，但包含trampoline和trapframe的映射），
  // 用户页表的主要作用
  // 地址空间隔离：

  // 每个进程都有自己独立的用户页表，提供独立的虚拟地址空间
  // 进程之间无法直接访问彼此的内存，提供了安全隔离
  // 用户进程无法直接访问内核内存（除非通过系统调用）
  // 虚拟内存管理：

  // 将进程的虚拟地址映射到物理内存页面
  // 允许进程使用连续的虚拟地址，即使物理内存是分散的
  // 支持内存分配、释放和扩展（如 sbrk 系统调用）
  //     权限控制：

  //         通过页表项的权限位（PTE_R,
  //     PTE_W, PTE_X, PTE_U）控制内存访问权限 代码段可以设置为只读可执行，数据段可读写，栈可读写等 用户页（PTE_U）只能被用户进程访问，内核页只能被内核访问 p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p); // 创建失败，释放资源
    release(&p->lock);
    return 0;
  }

  // 初始化进程的内核上下文。
  // 当这个进程第一次被调度时，它将从 `forkret` 函数开始执行。
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;    // 设置返回地址为 forkret
  p->context.sp = p->kstack + PGSIZE; // 设置内核栈指针指向栈顶

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// --------------------------------------------------------------------
// userinit
// --------------------------------------------------------------------
// 创建并初始化第一个用户进程 (initcode)。
// 这个进程将执行一小段代码，最终调用 `exec` 来运行 `/init` 程序。
// --------------------------------------------------------------------
void userinit(void)
{
  struct proc *p;

  // 分配一个进程
  p = allocproc();
  initproc = p;

  // 设置当前工作目录为根目录 "/"
  p->cwd = namei("/");

  // 将进程状态设置为可运行
  p->state = RUNNABLE;

  // 释放进程锁，上面allocproc()会先获取进程锁，防止重复修改进程
  release(&p->lock);
}

// --------------------------------------------------------------------
// growproc
// --------------------------------------------------------------------
// 增加或减少当前进程的用户内存。
// @param n: 内存变化的字节数。正数表示增加，负数表示减少。
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    // 增加内存
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    // 减少内存
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// --------------------------------------------------------------------
// kfork
// --------------------------------------------------------------------
// 创建一个新进程，作为当前进程的子进程。
// 新进程的状态几乎是父进程的精确副本。
// @return: 成功则在父进程中返回子进程的PID，在子进程中返回0。失败则返回-1。
// --------------------------------------------------------------------
int kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // 分配一个新的进程结构体
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // 复制父进程的用户内存到子进程
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // 复制父进程的陷阱帧（保存的用户寄存器）
  *(np->trapframe) = *(p->trapframe);

  // 在子进程中，fork系统调用的返回值应该是0。
  // a0 寄存器用于存放函数返回值。
  np->trapframe->a0 = 0;

  // 复制父进程的文件描述符表
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  // 复制当前工作目录的inode
  np->cwd = idup(p->cwd);

  // 复制进程名
  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  // 释放新进程的锁，因为它现在可以被调度了
  release(&np->lock);

  // 设置父子关系，需要 wait_lock 保护
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  // 将子进程状态设置为可运行
  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// --------------------------------------------------------------------
// reparent
// --------------------------------------------------------------------
// 将一个进程的所有子进程过继给 `init` 进程。
// 当一个进程退出时，它的子进程就变成了孤儿进程，需要由 `init` 进程来接管。
// @param p: 正在退出的进程。
// @note: 调用者必须持有 `wait_lock`。
// --------------------------------------------------------------------
void reparent(struct proc *p)
{
  struct proc *pp;

  // 遍历所有进程
  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    // 如果找到一个子进程
    if (pp->parent == p)
    {
      // 将其父进程设置为 initproc
      pp->parent = initproc;
      // 唤醒可能正在 wait() 的 initproc
      wakeup(initproc);
    }
  }
}

// --------------------------------------------------------------------
// kexit
// --------------------------------------------------------------------
// 退出当前进程。此函数不会返回。
// 退出的进程会进入 `ZOMBIE` 状态，直到其父进程调用 `wait()` 来回收它。
// @param status: 退出状态码，会传递给父进程。
// --------------------------------------------------------------------
void kexit(int status)
{
  struct proc *p = myproc();

  // init 进程不允许退出
  if (p == initproc)
    panic("init exiting");

  // 关闭所有打开的文件
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  // 释放当前工作目录
  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 将所有子进程过继给 init 进程
  reparent(p);

  // 唤醒正在 wait() 的父进程
  wakeup(p->parent);

  acquire(&p->lock);

  // 设置退出状态和进程状态
  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // 切换到调度器，永不返回
  sched();
  panic("zombie exit");
}

// --------------------------------------------------------------------
// kwait
// --------------------------------------------------------------------
// 等待一个子进程退出，并返回其PID。
// @param addr: 用户空间地址，用于接收子进程的退出状态码。如果为0，则不接收。
// @return: 成功则返回退出的子进程的PID，如果没有子进程则返回-1。
// --------------------------------------------------------------------
int kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  { // 无限循环，直到找到一个退出的子进程或没有子进程
    // 扫描进程表，寻找已退出的子进程
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // 确保子进程不是正在退出或切换中
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // 找到了一个僵尸子进程
          pid = pp->pid;
          // 如果用户提供了地址，则将退出状态码复制到用户空间
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          // 释放僵尸进程的资源
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // 如果没有任何子进程，或者当前进程被杀死了，则无需等待
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // 等待一个子进程退出
    // sleep 会原子地释放 wait_lock 并使当前进程休眠
    sleep(p, &wait_lock);
  }
}

// --------------------------------------------------------------------
// scheduler
// --------------------------------------------------------------------
// 每个CPU核心的进程调度器。
// 每个CPU在完成自身的初始化后都会调用此函数，并且永不返回。
// 调度器循环执行以下操作：
//  - 选择一个可运行的（RUNNABLE）进程。
//  - 使用 `swtch` 切换到该进程的上下文开始运行。
//  - 最终，该进程会通过 `swtch` 将控制权交还给调度器。
// --------------------------------------------------------------------
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    // 启用中断，以避免在所有进程都在等待I/O时发生死锁。
    // 这是一个临时的启用，很快会再次禁用。
    intr_on();
    intr_off();

    int found = 0;
    // 遍历进程表，寻找一个可运行的进程
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // 找到了一个可运行的进程，准备切换
        p->state = RUNNING;
        c->proc = p;

        // 上下文切换：保存当前调度器上下文到 c->context，
        // 并从 p->context 恢复目标进程的上下文。
        swtch(&c->context, &p->context);

        // 当进程将控制权交还给调度器时，代码会从这里继续执行。
        // 此时，当前CPU上没有进程在运行。
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0)
    {
      // 如果没有找到可运行的进程，CPU进入低功耗状态，等待中断唤醒。
      asm volatile("wfi");
    }
  }
}

// --------------------------------------------------------------------
// sched
// --------------------------------------------------------------------
// 将当前进程的执行上下文切换到调度器上下文。
// 调用此函数前，必须持有当前进程的锁 `p->lock`，并且进程状态不能是 `RUNNING`。
// @note: 这个函数不会返回，直到调度器再次调度该进程。
// --------------------------------------------------------------------
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  // 一系列的检查，确保调用 sched 的时机是正确的
  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  // 保存当前的中断启用状态
  intena = mycpu()->intena;
  // 切换到调度器上下文
  swtch(&p->context, &mycpu()->context);
  // 当进程被再次调度回来时，恢复之前的中断状态
  mycpu()->intena = intena;
}

// --------------------------------------------------------------------
// yield
// --------------------------------------------------------------------
// 当前进程主动放弃CPU，让出执行权给其他进程。
// --------------------------------------------------------------------
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  // 将进程状态设置为可运行，以便调度器可以再次选择它
  p->state = RUNNABLE;
  // 调用 sched 切换到调度器
  sched();
  // 当进程被重新调度回来后，释放锁
  release(&p->lock);
}

// --------------------------------------------------------------------
// forkret
// --------------------------------------------------------------------
// fork出的子进程第一次被调度时，会从这里开始执行。
// 这个函数负责完成一些只能在进程上下文中进行的初始化工作，
// 然后返回到用户空间。
// --------------------------------------------------------------------
void forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // 从调度器切换过来时，仍然持有进程锁，需要释放它
  release(&p->lock);

  if (first)
  {
    // 文件系统的初始化必须在常规进程的上下文中运行（因为它可能会调用sleep），
    // 因此不能在 main() 中直接运行。
    // 第一个被创建的进程（initproc）会负责执行这里的初始化。
    fsinit(ROOTDEV);

    first = 0;
    // 内存屏障，确保其他核心能看到 first=0
    __sync_synchronize();

    // 现在文件系统已经初始化，可以调用 kexec 来加载 init 程序
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1)
    {
      panic("exec");
    }
  }

  // 模拟从陷阱返回，切换到用户空间执行
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// --------------------------------------------------------------------
// sleep
// --------------------------------------------------------------------
// 使当前进程在一个特定的“通道”（channel）上休眠，并原子地释放一个锁。
// 当进程被唤醒后，它会重新获取这个锁。
// @param chan: 睡眠通道(Channel 通道指针)，通常取等待资源或条件变量的地址作为标识；仅充当匹配键，无需指向有效数据。
// @param lk: 一个条件锁，在进程休眠前释放，唤醒后重新获取。
// --------------------------------------------------------------------
void sleep(void *chan, struct spinlock *lk) // chan(Channel 通道指针，用作等待队列的标识，可指向任意地址，未必解引用)
{
  struct proc *p = myproc();

  // 必须先获取进程自身的锁 p->lock，才能安全地修改进程状态并调用调度器。
  // 一旦持有了 p->lock，就可以保证不会错过任何 wakeup 调用，
  // 因为 wakeup 也会获取 p->lock。这时释放外部传入的锁 lk 是安全的。
  acquire(&p->lock);
  release(lk);

  // 进入休眠状态
  // p->chan 记录了进程正在等待的“通道”或事件。
  // 当其他进程完成某个操作并调用 wakeup(chan) 时，
  // 调度器会查找所有在相同 chan 上睡眠的进程并唤醒它们。
  p->chan = chan;
  // 将进程状态设置为 SLEEPING，表示该进程暂时不参与 CPU 调度。
  p->state = SLEEPING;

  // 切换到调度器
  sched();

  // 当进程被唤醒并重新调度后，代码从这里继续执行。
  // 清理工作：清除睡眠通道。
  p->chan = 0;

  // 重新获取之前释放的锁
  release(&p->lock);
  acquire(lk);
}

// --------------------------------------------------------------------
// wakeup
// --------------------------------------------------------------------
// 唤醒所有在指定通道上休眠的进程。
// @param chan: 要唤醒的睡眠通道标识。通常取资源地址作为等待键值。
// @note: 调用者通常应该持有与该通道相关的锁。
// --------------------------------------------------------------------
void wakeup(void *chan) // chan(Channel 通道指针) 标识要唤醒的等待事件，可以是任意地址值
{
  struct proc *p;

  // 遍历所有进程
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    { // 不唤醒自己
      acquire(&p->lock);
      // 如果进程正在指定的通道上休眠
      if (p->state == SLEEPING && p->chan == chan)
      {
        // 将其状态改为可运行
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// --------------------------------------------------------------------
// kkill
// --------------------------------------------------------------------
// 杀死指定PID的进程。
// 被杀死的进程不会立即退出，而是在下一次返回用户空间时（在 trap.c 的 usertrap 中）检查 `killed` 标志并退出。
// @param pid: 要杀死的进程的PID。
// @return: 成功返回0，如果找不到该进程则返回-1。
// --------------------------------------------------------------------
int kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      // 设置 killed 标志
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // 如果进程正在休眠，唤醒它，以便它能尽快退出
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// --------------------------------------------------------------------
// setkilled
// --------------------------------------------------------------------
// 设置一个进程的 `killed` 标志。
// @param p: 目标进程。
// --------------------------------------------------------------------
void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

// --------------------------------------------------------------------
// killed
// --------------------------------------------------------------------
// 检查一个进程是否被标记为 `killed`。
// @param p: 目标进程。
// @return: 如果被杀死则返回1，否则返回0。
// --------------------------------------------------------------------
int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// --------------------------------------------------------------------
// either_copyout
// --------------------------------------------------------------------
// 从内核空间复制数据到用户空间或内核空间的另一个地址。
// @param user_dst: 如果为1，则目标地址 `dst` 是用户虚拟地址；否则是内核地址。
// @param dst: 目标地址。
// @param src: 源地址（在内核空间）。
// @param len: 要复制的字节数。
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// --------------------------------------------------------------------
// either_copyin
// --------------------------------------------------------------------
// 从用户空间或内核空间复制数据到内核空间。
// @param dst: 目标地址（在内核空间）。
// @param user_src: 如果为1，则源地址 `src` 是用户虚拟地址；否则是内核地址。
// @param src: 源地址。
// @param len: 要复制的字节数。
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// --------------------------------------------------------------------
// procdump
// --------------------------------------------------------------------
// 打印进程列表到控��台，用于调试。
// 当用户在控制台按下 Ctrl+P 时运行。
// @note: 这个函数没有加锁，以避免在一个已经卡住的机器上进一步造成死锁。
// --------------------------------------------------------------------
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
