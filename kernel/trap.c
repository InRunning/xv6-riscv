// ====================================================================
// 1. INCLUDES & GLOBAL VARIABLES
// ====================================================================
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock; // 保护 `ticks` 变量的锁
uint ticks;                // 记录时钟中断的次数

extern char trampoline[], uservec[]; // 在 trampoline.S 中定义

// kernelvec 在 kernelvec.S 中定义，是内核态陷阱处理的入口点
void kernelvec();

extern int devintr(); // 声明设备中断处理函数

// ====================================================================
// 2. FUNCTIONS
// ====================================================================

// --------------------------------------------------------------------
// trapinit
// --------------------------------------------------------------------
// 初始化陷阱处理相关的锁。
// --------------------------------------------------------------------
void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// --------------------------------------------------------------------
// trapinithart
// --------------------------------------------------------------------
// 在每个CPU核心上设置内核态的陷阱向量。
// `stvec` 寄存器指向陷阱处理程序的地址。
// --------------------------------------------------------------------
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

// --------------------------------------------------------------------
// usertrap
// --------------------------------------------------------------------
// 处理来自用户空间的中断、异常或系统调用。
// 这个函数由 `trampoline.S` 中的 `uservec` 调用。
// @return: 返回用户页表的 `satp` 值，供 `trampoline.S` 切换页表并返回用户空间。
// --------------------------------------------------------------------
uint64
usertrap(void)
{
  int which_dev = 0;

  // 检查 sstatus 寄存器的 SPP 位，确保我们是从用户模式进入的陷阱
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // 切换陷阱向量到 `kernelvec`，这样在内核中发生的中断或异常
  // 将由 `kerneltrap` 处理，而不是再次进入 `usertrap`。
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // 保存用户程序的程序计数器（PC），以便之后可以返回
  p->trapframe->epc = r_sepc();
  
  // `scause` 寄存器存放陷阱的原因
  if(r_scause() == 8){
    // 原因 8: 来自用户态的系统调用 (ecall)

    if(killed(p))
      kexit(-1);

    // `sepc` 指向 `ecall` 指令本身，返回时应该执行下一条指令，所以 `epc` + 4
    p->trapframe->epc += 4;

    // 现在我们已经处理完 `sepc`, `scause` 等寄存器，可以安全地开启中断
    intr_on();

    // 执行系统调用处理函数
    syscall();
  } else if((which_dev = devintr()) != 0){
    // 是一个设备中断
  } else if((r_scause() == 15 || r_scause() == 13) &&
            vmfault(p->pagetable, r_stval(), (r_scause() == 13)? 1 : 0) != 0) {
    // 原因 13 (Load page fault) 或 15 (Store page fault)
    // 并且页面错误是由惰性分配引起的，且 `vmfault` 处理成功
  } else {
    // 未知的陷阱类型
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p); // 杀死该进程
  }

  // 检查进程是否在处理陷阱期间被杀死
  if(killed(p))
    kexit(-1);

  // 如果是时钟中断，则当前进程主动放弃CPU，进行一次调度
  if(which_dev == 2)
    yield();

  // 准备返回用户空间，设置相关的控制寄存器和陷阱帧
  prepare_return();

  // 返回用户页表的 satp 值
  uint64 satp = MAKE_SATP(p->pagetable);

  return satp;
}

// --------------------------------------------------------------------
// prepare_return
// --------------------------------------------------------------------
// 在从陷阱返回用户空间之前，设置好陷阱帧和相关的控制寄存器。
// --------------------------------------------------------------------
void
prepare_return(void)
{
  struct proc *p = myproc();

  // 我们即将把陷阱目标从 `kerneltrap` 切换回 `usertrap`。
  // 为��防止在内核代码中发生陷阱而被错误地导向 `usertrap`，
  // 必须先禁用中断。
  intr_off();

  // 将 `stvec` 设置为 `uservec` 的地址，以便下一次来自用户空间的陷阱
  // 能够被正确处理。
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // 设置陷阱帧中的内核相关信息，供下一次陷阱时 `uservec` 使用。
  p->trapframe->kernel_satp = r_satp();         // 内核页表
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // 进程的内核栈顶
  p->trapframe->kernel_trap = (uint64)usertrap; // `usertrap` 函数地址
  p->trapframe->kernel_hartid = r_tp();         // 当前 hart id

  // 设置 `sret` 指令返回用户空间时需要的寄存器。
  
  // 设置 `sstatus` 寄存器：
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // 清除 SPP 位，表示下一次 `sret` 将返回到 User 模式
  x |= SSTATUS_SPIE; // 设置 SPIE 位，以便在返回用户模式后开启中断
  w_sstatus(x);

  // 设置 `sepc`，即异常程序计数器，指向之前保存的用户PC，
  // 这样 `sret` 就会从那里继续执行。
  w_sepc(p->trapframe->epc);
}

// --------------------------------------------------------------------
// kerneltrap
// --------------------------------------------------------------------
// 处理在内核（supervisor）模式下发生的中断和异常。
// --------------------------------------------------------------------
void
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  // 确保我们是从 supervisor 模式进入的
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  // 确保中断是禁用的
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  // 检查是否是设备中断
  if((which_dev = devintr()) == 0){
    // 未知的陷阱源
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // 如果是时钟中断，并且当前有进程在运行，则让出CPU
  if(which_dev == 2 && myproc() != 0)
    yield();

  // `yield()` 可能会导致上下文切换，其中可能也包含陷阱。
  // 因此，需要恢复 `sepc` 和 `sstatus`，以确保 `sret` 能正确返回到
  // `kerneltrap` 被中断的地方。
  w_sepc(sepc);
  w_sstatus(sstatus);
}

// --------------------------------------------------------------------
// clockintr
// --------------------------------------------------------------------
// 处理时钟中断。
// --------------------------------------------------------------------
void
clockintr()
{
  // 只在CPU 0上更新时钟计数器
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks); // 唤醒所有等待时钟的进程
    release(&tickslock);
  }

  // 设置下一次时钟中断。
  // 这也会清除当前的定时器中断请求。
  // 1000000 大约是 0.1 秒。
  w_stimecmp(r_time() + 1000000);
}

// --------------------------------------------------------------------
// devintr
// --------------------------------------------------------------------
// 检查并处理外部设备中断或软件中断。
// @return: 2 表示时钟中断, 1 表示其他设备中断, 0 表示无法识别。
// --------------------------------------------------------------------
int
devintr()
{
  uint64 scause = r_scause();

  // 检查 `scause` 寄存器来判断中断类型
  if(scause == 0x8000000000000009L){
    // Supervisor 模式的外部中断 (来自 PLIC)

    // 从 PLIC 获取中断请求号 (IRQ)
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr(); // UART 中断
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr(); // VirtIO 磁盘中断
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // 通知 PLIC 该中断已处理完毕，允许该设备再次触发中断
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // Supervisor 模式的定时器中断
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

