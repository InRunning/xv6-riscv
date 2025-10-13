#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
// 为每个CPU分配一个栈空间，entry.S会使用
// 使用16字节对齐，这是RISC-V ABI的要求
// 每个CPU栈大小为4096字节（4KB），NCPU是CPU核心数
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// entry.S jumps here in machine mode on stack0.
// entry.S会在machine模式下跳转到这里，使用stack0作为栈
// 这是内核启动的第一个C函数，从汇编代码进入
void
start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  // 设置M Previous Privilege mode为Supervisor模式，为mret指令做准备
  // mret指令会根据mstatus寄存器中的MPP字段恢复到之前的特权模式
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;  // 清除MPP位域
  x |= MSTATUS_MPP_S;      // 设置为Supervisor模式
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // 设置M Exception Program Counter为main函数地址，为mret指令做准备
  // mret指令会跳转到mepc寄存器中指定的地址
  // requires gcc -mcmodel=medany - 编译时需要使用中等地址模型
  w_mepc((uint64)main);

  // disable paging for now.
  // 暂时禁用分页机制
  // satp寄存器的第63位为0表示禁用分页，使用直接映射的物理地址
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // 将所有中断和异常委托给Supervisor模式处理
  // medeleg: 将异常委托给S模式
  // mideleg: 将中断委托给S模式
  // sie: 启用S模式的外部中断和定时器中断
  w_medeleg(0xffff);      // 委托所有异常给Supervisor模式
  w_mideleg(0xffff);      // 委托所有中断给Supervisor模式
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);  // 启用S模式的外部中断和定时器中断

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  // 配置物理内存保护，允许Supervisor模式访问所有物理内存
  // PMP0配置为覆盖整个物理地址空间
  w_pmpaddr0(0x3fffffffffffffull);  // PMP地址0设置为最大物理地址
  w_pmpcfg0(0xf);                   // PMP配置0设置为RW权限，覆盖整个地址空间

  // ask for clock interrupts.
  // 请求时钟中断
  // 初始化定时器，使其能够产生周期性的时钟中断
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // 将每个CPU的hartid保存在tp寄存器中，供cpuid()函数使用
  // tp寄存器是RISC-V的临时寄存器，用于保存线程指针
  int id = r_mhartid();  // 读取当前CPU的hartid
  w_tp(id);              // 将hartid写入tp寄存器

  // switch to supervisor mode and jump to main().
  // 切换到Supervisor模式并跳转到main()函数
  // 使用mret指令从machine模式返回到Supervisor模式
  // 这会跳转到之前设置的mepc寄存器中的main()函数地址
  asm volatile("mret");
}

// ask each hart to generate timer interrupts.
// 请求每个hart生成定时器中断
// 这个函数为当前hart配置定时器中断
void
timerinit()
{
  // enable supervisor-mode timer interrupts.
  // 启用Supervisor模式的定时器中断
  // 设置MIE寄存器的STIE位，允许Supervisor模式响应定时器中断
  w_mie(r_mie() | MIE_STIE);
  
  // enable the sstc extension (i.e. stimecmp).
  // 启用SSTC扩展（即stimecmp功能）
  // SSTC (Supervisor-mode Software Timer Counter Compare) 允许S模式设置定时器比较值
  // 设置MENVCFG寄存器的第63位来启用SSTC扩展
  w_menvcfg(r_menvcfg() | (1L << 63));
  
  // allow supervisor to use stimecmp and time.
  // 允许Supervisor模式使用stimecmp和time指令
  // 设置MCOUNTEREN寄存器的第1位，使S模式可以访问time和stimecmp寄存器
  w_mcounteren(r_mcounteren() | 2);
  
  // ask for the very first timer interrupt.
  // 请求第一个定时器中断
  // 设置stimecmp寄存器为当前时间+1000000个时钟周期
  // 这会在大约1秒后触发第一个定时器中断（假设时钟频率为1MHz）
  w_stimecmp(r_time() + 1000000);
}
