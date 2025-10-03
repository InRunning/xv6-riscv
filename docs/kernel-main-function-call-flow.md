# kernel/main.c 中的 main() 函数执行流程

本文档描述了 xv6 内核中 `main()` 函数的执行流程，从系统启动到调度器运行的完整过程。

## 1. 函数入口点

`main()` 函数是 xv6 内核的入口点，由 `start()` 函数在所有 CPU 上以 supervisor 模式跳转而来。这个函数负责初始化内核的各个子系统，并最终启动调度器。

## 2. 主 CPU (CPU 0) 初始化流程

只有第一个启动的 CPU（hart 0）执行主要的内核初始化工作：

1. **控制台初始化** - [`consoleinit()`](../kernel/console.c#227)
   - 初始化控制台的自旋锁
   - 初始化 UART 硬件
   - 注册控制台设备驱动到设备开关表

2. **printf 初始化** - [`printfinit()`](../kernel/printf.c#76)
   - 初始化内核的 printf 函数，主要是初始化一个锁

3. **打印启动信息** - [`printf()`](../kernel/printf.c#76)
   - 打印 "xv6 kernel is booting" 消息

4. **物理内存分配器初始化** - [`kinit()`](../kernel/kalloc.c#43)
   - 初始化内存管理器的锁
   - 将从内核结束到物理内存顶部的所有内存加入空闲链表

5. **内核页表初始化** - [`kvminit()`](../kernel/vm.c#85)
   - 创建内核页表，建立直接映射
   - 映射硬件设备、内核代码/数据和 trampoline 页面

6. **启用分页** - [`kvminithart()`](../kernel/vm.c#96)
   - 在当前 CPU 上启用分页机制
   - 设置 satp 寄存器并刷新 TLB

7. **进程表初始化** - [`procinit()`](../kernel/proc.c#69)
   - 初始化进程表和相关锁
   - 初始化每个进程槽和锁

8. **中断处理初始化** - [`trapinit()`](../kernel/trap.c#32)
   - 初始化陷阱处理相关的锁

9. **安装内核陷阱向量** - [`trapinithart()`](../kernel/trap.c#44)
   - 在当前 CPU 上设置内核态的陷阱向量

10. **中断控制器初始化** - [`plicinit()`](../kernel/plic.c#174)
    - 初始化 PLIC（平台级中断控制器）

11. **配置 PLIC** - [`plicinithart()`](../kernel/plic.c#175)
    - 为当前 CPU 配置 PLIC 以接收设备中断

12. **缓冲区缓存初始化** - [`binit()`](../kernel/bio.c#41)
    - 初始化缓冲区缓存的数据结构
    - 创建循环双向链表管理所有缓冲区

13. **inode 缓存初始化** - [`iinit()`](../kernel/fs.c#186)
    - 初始化 inode 缓存和相关锁

14. **文件表初始化** - [`fileinit()`](../kernel/file.c#23)
    - 初始化文件表和锁

15. **虚拟磁盘初始化** - [`virtio_disk_init()`](../kernel/virtio_disk.c#64)
    - 初始化 VirtIO 磁盘设备
    - 设置磁盘队列和描述符

16. **创建第一个用户进程** - [`userinit()`](../kernel/proc.c#277)
    - 创建并初始化第一个用户进程 (initcode)
    - 设置进程状态为可运行

17. **设置启动完成标志**
    - 使用内存屏障确保前面的写操作对其他 CPU 可见
    - 设置 `started = 1` 标志，允许其他 CPU 继续执行

## 3. 其他 CPU 初始化流程

其他 CPU（harts > 0）等待主 CPU 完成初始化：

1. **等待主 CPU 初始化完成**
   - 自旋等待 `started` 标志被设置为 1
   - 使用内存屏障确保读取到 `started` 的最新值

2. **打印启动信息**
   - 打印 "hart X starting" 消息

3. **启用分页** - [`kvminithart()`](../kernel/vm.c#96)
   - 在当前 CPU 上启用分页机制

4. **安装内核陷阱向量** - [`trapinithart()`](../kernel/trap.c#44)
   - 为当前 CPU 安装内核陷阱向量

5. **配置 PLIC** - [`plicinithart()`](../kernel/plic.c#175)
   - 为当前 CPU 配置 PLIC 以接收设备中断

## 4. 启动调度器

所有 CPU 完成初始化后，都会进入调度器循环：

1. **进入调度器** - [`scheduler()`](../kernel/proc.c#529)
   - 每个CPU核心的进程调度器
   - 循环查找可运行的进程并执行上下文切换

2. **永不返回**
   - 调度器函数永远不会返回，它会一直循环调度进程

## 5. 总结

`main()` 函数是 xv6 内核启动的核心，它负责：

1. 初始化所有必要的内核子系统
2. 为多核环境设置必要的同步机制
3. 创建第一个用户进程
4. 启动进程调度器，使系统开始正常运行

这个流程展示了从系统启动到多任务调度开始的完整过程，是理解 xv6 内核工作原理的关键。