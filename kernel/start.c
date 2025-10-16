#include "types.h"     // 类型定义头文件 (Type Definitions Header)
#include "param.h"     // 参数定义头文件 (Parameter Definitions Header)
#include "memlayout.h" // 内存布局头文件 (Memory Layout Header)
#include "riscv.h"     // RISC-V架构头文件 (RISC-V Architecture Header)
#include "defs.h"      // 函数声明头文件 (Function Declarations Header)

void main();      // 主函数声明 (Main Function Declaration)
void timerinit(); // 定时器初始化函数声明 (Timer Initialization Function Declaration)

// entry.S needs one stack per CPU.
// 为每个CPU分配一个栈空间，entry.S会使用
// 使用16字节对齐，这是RISC-V ABI (Application Binary Interface) 的要求
// 每个CPU栈大小为4096字节（4KB），NCPU是CPU核心数
__attribute__((aligned(16))) char stack0[4096 * NCPU]; // CPU0的栈空间

// entry.S jumps here in machine mode on stack0.
// entry.S会在machine模式下跳转到这里，使用stack0作为栈
// 这是内核启动的第一个C函数，从汇编代码进入
void start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  // 设置M Previous Privilege mode为Supervisor模式，为mret指令做准备
  // mret (Machine-mode RETurn) 指令会根据mstatus寄存器中的MPP (Machine Previous Privilege) 字段恢复到之前的特权模式
  unsigned long x = r_mstatus(); // 读取mstatus (Machine Status) 寄存器
  x &= ~MSTATUS_MPP_MASK;        // 清除MPP (Machine Previous Privilege) 位域
  // ~MSTATUS_MPP_MASK 是一个位掩码，其中MPP位域对应的位置为0，其他位为1
  // 按位与操作会将MPP位域清零，同时保持其他位不变
  x |= MSTATUS_MPP_S; // 设置为Supervisor模式
  // MSTATUS_MPP_S 是一个位掩码，其中Supervisor模式对应的位为1，其他位为0
  // 按位或操作会将Supervisor模式位置1，同时保持其他位不变
  w_mstatus(x); // 写入mstatus寄存器

  // set M Exception Program Counter to main, for mret.
  // 设置M Exception Program Counter为main函数地址，为mret指令做准备
  // mret指令会跳转到mepc (Machine Exception Program Counter) 寄存器中指定的地址
  // requires gcc -mcmodel=medany - 编译时需要使用中等地址模型
  w_mepc((uint64)main); // 设置mepc (Machine Exception Program Counter) 寄存器为main函数地址
  // mepc寄存器是RISC-V架构中的Machine Exception Program Counter寄存器
  // 它用于存储当发生异常或中断时，CPU应该跳转到的程序计数器值
  // 在这里，我们设置mepc为main函数的地址，这样当执行mret指令时，
  // CPU会从Machine模式切换到Supervisor模式，并跳转到main函数开始执行

  // disable paging for now.
  // 暂时禁用分页机制
  // satp (Supervisor Address Translation and Protection) 寄存器的第63位为0表示禁用分页，使用直接映射的物理地址
  w_satp(0); // 禁用分页，使用直接物理地址映射
  // 为什么要禁用分页：
  // 1. 简化启动过程：在内核启动初期，分页机制尚未配置，使用直接映射可以避免复杂的地址转换
  // 2. 避免页表错误：如果页表未正确初始化，启用分页会导致CPU访问无效的页表项，引发异常
  // 3. 性能考虑：地址转换需要访问内存中的页表，直接映射避免了额外的内存访问开销
  // 4. 调试便利性：直接映射使得物理地址和虚拟地址相同，便于调试和内存访问
  // 5. 内核初始化：在main()函数中，内核会逐步建立页表，然后才启用分页机制

  // delegate all interrupts and exceptions to supervisor mode.
  // 将所有中断和异常委托给Supervisor模式处理
  // medeleg (Machine Exception Delegation): 将异常委托给S模式
  // mideleg (Machine Interrupt Delegation): 将中断委托给S模式
  // sie (Supervisor Interrupt Enable): 启用S模式的外部中断和定时器中断
  w_medeleg(0xffff);                    // 委托所有异常给Supervisor模式
  w_mideleg(0xffff);                    // 委托所有中断给Supervisor模式
  w_sie(r_sie() | SIE_SEIE | SIE_STIE); // 启用S模式的外部中断和定时器中断

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  // 配置物理内存保护，允许Supervisor模式访问所有物理内存
  // PMP (Physical Memory Protection) 0配置为覆盖整个物理地址空间
  w_pmpaddr0(0x3fffffffffffffull); // PMP地址0设置为最大物理地址
  w_pmpcfg0(0xf);                  // PMP配置0设置为RW权限，覆盖整个地址空间

  // ask for clock interrupts.
  // 请求时钟中断
  // 初始化定时器，使其能够产生周期性的时钟中断
  timerinit(); // 调用定时器初始化函数

  // keep each CPU's hartid in its tp register, for cpuid().
  // 将每个CPU的hartid (Hardware Thread ID) 保存在tp (Thread Pointer) 寄存器中，供cpuid()函数使用
  // tp寄存器是RISC-V的临时寄存器，用于保存线程指针
  int id = r_mhartid(); // 读取当前CPU的hartid (Hardware Thread ID)
  w_tp(id);             // 将hartid写入tp (Thread Pointer) 寄存器

  // switch to supervisor mode and jump to main().
  // 切换到Supervisor模式并跳转到main()函数
  // 使用mret指令从machine模式返回到Supervisor模式
  // 这会跳转到之前设置的mepc寄存器中的main()函数地址
  asm volatile("mret"); // 执行mret指令，切换到Supervisor模式并跳转到main()
}

// ask each hart to generate timer interrupts.
// 请求每个hart生成定时器中断
// 这个函数为当前hart配置定时器中断
void timerinit()
{
  // enable supervisor-mode timer interrupts.
  // 启用Supervisor模式的定时器中断
  // 设置MIE (Machine Interrupt Enable) 寄存器的STIE (Supervisor Timer Interrupt Enable) 位，允许Supervisor模式响应定时器中断
  w_mie(r_mie() | MIE_STIE); // 启用Supervisor模式的定时器中断

  // enable the sstc extension (i.e. stimecmp).
  // 启用SSTC (Supervisor-mode Software Timer Counter Compare) 扩展（即stimecmp功能）
  // SSTC允许S模式设置定时器比较值
  // 设置MENVCFG (Machine Environment Configuration) 寄存器的第63位来启用SSTC扩展
  w_menvcfg(r_menvcfg() | (1L << 63)); // 启用SSTC扩展

  // allow supervisor to use stimecmp and time.
  // 允许Supervisor模式使用stimecmp和time指令
  // 设置MCOUNTEREN (Machine Counter Enable) 寄存器的第1位，使S模式可以访问time和stimecmp寄存器
  w_mcounteren(r_mcounteren() | 2); // 允许Supervisor模式访问time和stimecmp寄存器

  // ask for the very first timer interrupt.
  // 请求第一个定时器中断
  // 设置stimecmp (Supervisor Time Compare) 寄存器为当前时间+1000000个时钟周期
  // 这会在大约1秒后触发第一个定时器中断（假设时钟频率为1MHz）
  w_stimecmp(r_time() + 1000000); // 设置第一个定时器中断
}
