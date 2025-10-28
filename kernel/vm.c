// ====================================================================
// 1. INCLUDES & GLOBAL VARIABLES
// ====================================================================
// 包含参数定义头文件 (Parameter Header)
#include "param.h"
// 包含数据类型定义头文件 (Types Header)
#include "types.h"
// 包含内存布局定义头文件 (Memory Layout Header)
#include "memlayout.h"
// 包含可执行链接格式 (Executable and Linkable Format) 头文件
#include "elf.h"
// 包含 RISC-V 架构相关定义头文件
#include "riscv.h"
// 包含内核函数声明头文件 (Definitions Header)
#include "defs.h"
// 包含自旋锁定义头文件 (Spin Lock Header)
#include "spinlock.h"
// 包含进程相关定义头文件 (Process Header)
#include "proc.h"
// 包含文件系统相关定义头文件 (File System Header)
#include "fs.h"

// 内核页表 (Kernel Page Table)，所有 CPU 共享
pagetable_t kernel_pagetable;

extern char etext[];      // kernel.ld 中定义，指向内核代码段的末尾 (End of Text)
extern char trampoline[]; // trampoline.S 中定义，指向陷阱处理代码 (Trampoline)

// ====================================================================
// 2. FUNCTIONS
// ====================================================================

// --------------------------------------------------------------------
// kvmmake
// --------------------------------------------------------------------
// 创建内核页表。
// 这个页表为内核提供了一个直接映射（direct mapping），即虚拟地址等于物理地址。
// 同时，它也映射了I/O设备、内核代码/数据等。
// @return: 创建好的内核页表的根指针。
// --------------------------------------------------------------------
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  // 分配一个页面作为页表的根
  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // --- 映射硬件设备 ---
  // UART (串口)
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  // VirtIO 磁盘接口
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  // PLIC (平台级中断控制器)
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // --- 映射内核内存 ---
  // 映射内核代码段 (text)，权限为可读、可执行
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
  // 映射内核数据段 (data) 和剩余的所有物理内存，权限为可读、可写
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // --- 映射 Trampoline 页面 ---
  // 将用于陷阱处理的 trampoline 代码映射到内核虚拟地址空间的最高处
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // --- 为每个进程分配并映射内核栈 ---
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// --------------------------------------------------------------------
// kvmmap
// --------------------------------------------------------------------
// 在内核页表中添加一个映射。
// 这是一个 `mappages` 的简单包装，主要在启动时使用。
// @note: 这个函数不刷新TLB，也不启用分页。
// --------------------------------------------------------------------
void kvmmap(pagetable_t kpgtbl,   // 页表根指针，指向要操作的内核页表
            uint64 va,            // 起始虚拟地址，映射的目标虚拟地址
            uint64 pa,            // 起始物理地址，映射的源物理地址
            uint64 sz,            // 映射区域的大小（字节），必须是页对齐的
            int perm)             // 页表项权限位（PTE_R可读、PTE_W可写、PTE_X可执行）
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// --------------------------------------------------------------------
// kvminit
// --------------------------------------------------------------------
// 初始化内核页表。
// 这个函数在系统启动的早期被调用，且只被主CPU调用一次。
// --------------------------------------------------------------------

void kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// --------------------------------------------------------------------
// kvminithart
// --------------------------------------------------------------------
// 在当前CPU核心上启用分页机制。
// 每个CPU核心都需要调用这个函数来设置自己的 `satp` 寄存器。
// --------------------------------------------------------------------
void kvminithart()
{
  // 确保所有对页表内存的写操作都已完成
  sfence_vma();

  // 将内核页表的物理地址写入 satp 寄存器，以启用分页
  w_satp(MAKE_SATP(kernel_pagetable));

  // 刷新TLB，清除旧的、无效的转换条目
  sfence_vma();
}

// --------------------------------------------------------------------
// walk
// --------------------------------------------------------------------
// 在页表中查找给定虚拟地址 `va` 对应的页表项（PTE）的地址。
// 这是页表管理的核心函数。
// @param pagetable: 页表的根指针。
// @param va: 要查找的虚拟地址。
// @param alloc: 如果为1，则在查找过程中如果遇到缺失的页表页，会分配新的页表页。
// @return: 成功则返回指向PTE的指针，失败则返回0。
// --------------------------------------------------------------------
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  // 检查虚拟地址是否在合法范围内
  if (va >= MAXVA)
    panic("walk");

  // RISC-V Sv39 分页方案使用三级页表。
  // 这个循环从最高级（L2）开始，逐级向下查找。
  for (int level = 2; level > 0; level--)
  {
    // PX宏从va中提取当前级别的页表索引
    pte_t *pte = &pagetable[PX(level, va)];

    if (*pte & PTE_V)
    {
      // 如��PTE是有效的，说明它指向下一级的页表。
      // PTE2PA从PTE中提取物理地址，并将其作为下一级页表的基地址。
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      // 如果PTE无效，且 alloc 参数为真，则需要分配一个新的页表页。
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0; // 分配失败或不允许分配

      // 新分配的页表页需要清零
      memset(pagetable, 0, PGSIZE);

      // 在当前PTE中填入新分配的页表页的物理地址，并设置有效位。
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }

  // 返回最低一级（L0）页表中对应va的PTE地址。
  return &pagetable[PX(0, va)];
}

// --------------------------------------------------------------------
// walkaddr
// --------------------------------------------------------------------
// 查找一个虚拟地址对应的物理地址。
// @param pagetable: 页表的根指针。
// @param va: 要查找的虚拟地址。
// @return: 成功则返回物理地址，如果未映射则返回0。
// @note: 只能用于查找用户页。
// --------------------------------------------------------------------
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  // 查找PTE，但不分配新页表
  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  // 检查PTE是否有效
  if ((*pte & PTE_V) == 0)
    return 0;
  // 检查PTE是否允许用户访问
  if ((*pte & PTE_U) == 0)
    return 0;

  // 从PTE中提取物理地址
  pa = PTE2PA(*pte);
  return pa;
}

// --------------------------------------------------------------------
// mappages
// --------------------------------------------------------------------
// 创建一组页表项，将一段连续的虚拟地址映射到一段连续的物理地址。
// @param pagetable: 页表的根指针。
// @param va: 起始虚拟地址。
// @param size: 映射区域的大小（字节）。
// @param pa: 起始物理地址。
// @param perm: 页表项的权限位 (PTE_R, PTE_W, PTE_X, PTE_U)。
// @return: 成功返回0，失败返回-1。
// @note: `va` 和 `size` 必须是页对齐的。
// --------------------------------------------------------------------
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");
  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");
  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;)
  {
    // 查找（或创建）当前虚拟地址 `a` 对应的PTE
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;

    // 检查该地址是否已经被映射，防止重复映射
    if (*pte & PTE_V)
      panic("mappages: remap");

    // 填充PTE：物理地址 + 权限位 + 有效位
    *pte = PA2PTE(pa) | perm | PTE_V;

    if (a == last)
      break;

    // 移动到下一个页面
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// --------------------------------------------------------------------
// uvmcreate
// --------------------------------------------------------------------
// 创建一个空的用户页表。
// @return: 成功则返回页表根指针，失败（内存不足）则返回0。
// --------------------------------------------------------------------
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  // 分配一个页面作为页表的根
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  // 清零
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// --------------------------------------------------------------------
// uvmunmap
// --------------------------------------------------------------------
// 解除一段虚拟地址的映射。
// @param pagetable: 页表的根指针。
// @param va: 起始虚拟地址，必须页对齐。
// @param npages: 要解除映射的页面数量。
// @param do_free: 如果为1，则同时释放这些页面对应的物理内存。
// --------------------------------------------------------------------
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    // 查找PTE，如果中间的页表页不存在，则直接跳过
    if ((pte = walk(pagetable, a, 0)) == 0)
      continue;
    // 如果PTE无效（即没有映射物理页），也跳过
    if ((*pte & PTE_V) == 0)
      continue;

    if (do_free)
    {
      // 释放物理内存
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    // 将PTE清零，使其无效
    *pte = 0;
  }
}

// --------------------------------------------------------------------
// uvmalloc
// --------------------------------------------------------------------
// 为进程分配并映射用户内存，使其大小从 `oldsz` 增长到 `newsz`。
// @param pagetable: 页表的根指针。
// @param oldsz: 旧的内存大小。
// @param newsz: 新的内存大小。
// @param xperm: 额外的权限位（通常是PTE_W）。
// @return: 成功则返回 `newsz`，失败则返回0。
// --------------------------------------------------------------------
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  // 将旧大小向上对齐到页面边界
  oldsz = PGROUNDUP(oldsz);
  // 逐页进行分配和映射
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc(); // 分配一页物理内存
    if (mem == 0)
    {
      // 分配失败，回滚已分配的内存
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE); // 清零
    // 创建映射
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// --------------------------------------------------------------------
// uvmdealloc
// --------------------------------------------------------------------
// 释放用户内存，使进程大小从 `oldsz` 减小到 `newsz`。
// @param pagetable: 页表的根指针。
// @param oldsz: 旧的内存大小。
// @param newsz: 新的内存大小。
// @return: 返回 `newsz`。
// --------------------------------------------------------------------
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  // 如果新旧大小的页对齐边界不同，则说明有整页可以释放
  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    // 从新的页边界开始，解除映射并释放物理内存
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// --------------------------------------------------------------------
// freewalk
// --------------------------------------------------------------------
// 递归地释放一个页表的所有页表页。
// @param pagetable: 要释放的页表的根指针。
// @note: 在调用此函数之前，所有叶子节点的映射（指向物理内存页的PTE）
//        必须已经被 `uvmunmap` 解除。
// --------------------------------------------------------------------
void freewalk(pagetable_t pagetable)
{
  // 一个页表页包含 512 个PTE
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    // 检查PTE是否指向下一级页表。
    // 判断条件是：PTE有效，但R,W,X权限位都为0。
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // 这是一个指向下一级页表的PTE
      uint64 child = PTE2PA(pte);
      // 递归调用以释放子页表
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      // 如果PTE有效且不是指向下一级页表，那么它就是一个叶子节点。
      // 这不应该发生，因为我们假设所有叶子节点都已被解除映射。
      panic("freewalk: leaf");
    }
  }
  // 释放页表本身
  kfree((void *)pagetable);
}

// --------------------------------------------------------------------
// uvmfree
// --------------------------------------------------------------------
// 释放一个完整的用户页表，包括所有物理内存页和页表页。
// @param pagetable: 要释放的页表的根指针。
// @param sz: 进程的用户内存大小。
// --------------------------------------------------------------------
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    // 首先，解除所有用户页面的映射并释放物理内存
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  // 然后，递归地释放所有页表页
  freewalk(pagetable);
}

// --------------------------------------------------------------------
// uvmcopy
// --------------------------------------------------------------------
// 将父进程的内存完整地复制到子进程的页表中。
// 这包括复制物理内存内容和创建新的页表映射。
// @param old: 父进程的页表。
// @param new: 子进程的新页表（初始为空）。
// @param sz: 要复制的内存大小。
// @return: 成功返回0，失败返回-1。失败时会释放已分配的页面。
// --------------------------------------------------------------------
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  // 逐页进行复制
  for (i = 0; i < sz; i += PGSIZE)
  {
    // 在旧页表中查找PTE
    if ((pte = walk(old, i, 0)) == 0)
      continue; // 如果中间页表不存在，跳过
    if ((*pte & PTE_V) == 0)
      continue; // 如果物理页未映射，跳过

    pa = PTE2PA(*pte);       // 获取物理地址
    flags = PTE_FLAGS(*pte); // 获取权限位

    // 为子进程分配新的物理页面
    if ((mem = kalloc()) == 0)
      goto err;

    // 将父进程的物理页面内容复制到新页面
    memmove(mem, (char *)pa, PGSIZE);

    // 在子进程的新页表中创建映射
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  // 如果发生错误，回滚已为子进程分配的所有页面
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// --------------------------------------------------------------------
// uvmclear
// --------------------------------------------------------------------
// 清除一个PTE的用户访问权限位（PTE_U）。
// 这使得该页面对用户空间不可访问。
// 主要用于 `exec` 系统调用，在用户栈下方创建一个保护页面（guard page）。
// @param pagetable: 页表根指针。
// @param va: 目标虚拟地址。
// --------------------------------------------------------------------
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  // 通过位运算清除 PTE_U 标志
  *pte &= ~PTE_U;
}

// --------------------------------------------------------------------
// copyout
// --------------------------------------------------------------------
// 从内核空间安全地复制数据到用户空间的虚拟地址。
// @param pagetable: 目标用户进程的页表。
// @param dstva: 目标用户虚拟地址。
// @param src: 内核空间的源数据地址。
// @param len: 要复制的字节数。
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva); // 获取目标地址所在的页面基地址
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0); // 查找物理地址
    if (pa0 == 0)
    {
      // 如果页面未映射（惰性分配），尝试通过页面错误处理来分配它
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
      {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // 禁止向只读的用户页面（如代码段）写入数据
    if ((*pte & PTE_W) == 0)
      return -1;

    // 计算本次可以在当前页面内复制的字节数
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;

    // 执行复制
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    // 更新剩余长度、源地址和目标地址
    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// --------------------------------------------------------------------
// copyin
// --------------------------------------------------------------------
// 从用户空间的虚拟地址安全地复制数据到内核空间。
// @param pagetable: 源用户进程的页表。
// @param dst: 内核空间的目标地址。
// @param srcva: 源用户虚拟地址。
// @param len: 要复制的字节数。
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);       // 获取源地址所在的页面基地址
    pa0 = walkaddr(pagetable, va0); // 查找物理地址
    if (pa0 == 0)
    {
      // 如果页面未映射（惰性分配），尝试通过页面错误处理来分配它
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
      {
        return -1;
      }
    }

    // 计算本次可以在当前页面内复制的字节数
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;

    // 执行复制
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    // 更新剩余长度、目标地址和源地址
    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// --------------------------------------------------------------------
// copyinstr
// --------------------------------------------------------------------
// 从用户空间安全地复制一个以空字符结尾的字符串到内核空间。
// @param pagetable: 源用户进程的页表。
// @param dst: 内核空间的目标地址。
// @param srcva: 源用户虚拟地址。
// @param max: 最多复制的字节数。
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;

    // 计算本次可以在当前页面内复制的字节数
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    // 逐字节复制，直到遇到 '\0' 或达到最大长度
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

// --------------------------------------------------------------------
// vmfault
// --------------------------------------------------------------------
// 处理由惰性分配引起的页面错误。
// 当进程访问一个通过 `sbrk` 扩展但尚未分配物理内存的页面时，会调用此函数。
// @param pagetable: 页表根指针。
// @param va: 发生错误的虚拟地址。
// @param read: 1表示读错误，0表示写错误（当前未使用）。
// @return: 成功则返回新分配的物理��面的地址，失败则返回0。
// --------------------------------------------------------------------
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  // 检查地址是否在进程的合法内存范围内
  if (va >= p->sz)
    return 0;

  va = PGROUNDDOWN(va);
  // 检查页面是否已经被映射
  if (ismapped(pagetable, va))
  {
    return 0;
  }

  // 分配一页物理内存
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;

  memset((void *)mem, 0, PGSIZE);

  // 创建映射
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0)
  {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

// --------------------------------------------------------------------
// ismapped
// --------------------------------------------------------------------
// 检查一个虚拟地址是否已经被映射到物理内存。
// @param pagetable: 页表根指针。
// @param va: 要检查的虚拟地址。
// @return: 如果已映射则返回1，否则返回0。
// --------------------------------------------------------------------
int ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
  {
    return 0;
  }
  if (*pte & PTE_V)
  {
    return 1;
  }
  return 0;
}
