// ====================================================================
// 1. INCLUDES
// ====================================================================
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// ====================================================================
// 2. HELPER FUNCTIONS & SYSCALL DISPATCH
// ====================================================================

// --------------------------------------------------------------------
// fetchaddr
// --------------------------------------------------------------------
// 从当前进程的用户空间获取一个 uint64 类型的地址。
// @param addr: 用户空间的虚拟地址。
// @param ip: 用于存放结果的内核空间指针。
//           注：ip是 "Integer Pointer" 的缩写，表示指向整型（在此为uint64类型）的指针
//           在这里，ip指向一个uint64类型的变量，用于存储从用户空间获取的地址值
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
// [fetchaddr](#fetchaddr)
int fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if (addr >= p->sz || addr + sizeof(uint64) > p->sz) // 检查地址是否在进程的合法范围内
    return -1;
  if (copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// --------------------------------------------------------------------
// fetchstr
// --------------------------------------------------------------------
// 从当前进程的用户空间获取一个以空字符结尾的字符串。
// @param addr: 用户空间的虚拟地址。
// @param buf: 用于存放结果的内核空间缓冲区。
// @param max: 缓冲区的最大长度。
// @return: 成功则返回字符串长度（不包括'\0'），失败返回-1。
// --------------------------------------------------------------------
// [fetchstr](#fetchstr)
int fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if (copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

// --------------------------------------------------------------------
// argraw
// --------------------------------------------------------------------
// 从陷阱帧中获取第 n 个原始的、未经处理的64位系统调用参数。
//
// 在RISC-V架构中，系统调用参数通过寄存器传递：
// - a7寄存器：系统调用号（在syscall()函数中处理）
// - a0寄存器：第一个参数
// - a1寄存器：第二个参数
// - a2寄存器：第三个参数
// - a3寄存器：第四个参数
// - a4寄存器：第五个参数
// - a5寄存器：第六个参数
//
// 当用户程序执行ecall指令触发系统调用时，这些寄存器的值会被保存到
// 进程的陷阱帧(trapframe)中，本函数从陷阱帧中提取这些参数值。
//
// @param n: 参数的索引 (0-5)，对应a0到a5寄存器。
// @return: 参数的值。
// --------------------------------------------------------------------
// [argraw](#argraw)
static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n)
  {
  case 0:
    // 返回第一个参数，存储在a0寄存器中
    // 例如：对于write(fd, buf, n)，a0存储文件描述符fd
    return p->trapframe->a0;
  case 1:
    // 返回第二个参数，存储在a1寄存器中
    // 例如：对于write(fd, buf, n)，a1存储缓冲区地址buf
    //       对于read(fd, buf, n)，a1存储缓冲区地址buf
    //       对于open(filename, flags)，a1存储文件打开标志flags
    return p->trapframe->a1;
  case 2:
    // 返回第三个参数，存储在a2寄存器中
    // 例如：对于write(fd, buf, n)，a2存储要写入的字节数n
    return p->trapframe->a2;
  case 3:
    // 返回第四个参数，存储在a3寄存器中
    return p->trapframe->a3;
  case 4:
    // 返回第五个参数，存储在a4寄存器中
    return p->trapframe->a4;
  case 5:
    // 返回第六个参数，存储在a5寄存器中
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// --------------------------------------------------------------------
// argint
// --------------------------------------------------------------------
// 获取第 n 个32位整型系统调用参数。
//
// 此函数从陷阱帧中获取第n个参数（存储在a0-a5寄存器中），
// 并将其作为32位整型值返回。实际参数存储在64位寄存器中，
// 但被截断为32位整型。
//
// @param n: 参数索引（0-5），对应a0-a5寄存器
// @param ip: 指向存储结果的整型变量的指针
//
// 示例：
// 对于getpid()系统调用（无参数），不需要使用此函数
// 对于kill(pid)系统调用，使用argint(0, &pid)获取第一个参数
// --------------------------------------------------------------------
// [argint](#argint)
void argint(int n, int *ip)
{
  *ip = argraw(n);
}

// --------------------------------------------------------------------
// argaddr
// --------------------------------------------------------------------
// 获取第 n 个指针类型的系统调用参数。
//
// 此函数从陷阱帧中获取第n个参数（存储在a0-a5寄存器中），
// 并将其作为64位地址值返回。这个地址是用户空间的虚拟地址，
// 内核在访问前需要检查其合法性。
//
// @param n: 参数索引（0-5），对应a0-a5寄存器
// @param ip: 指向存储结果的地址变量的指针
//           注：ip是 "Integer Pointer" 的缩写，表示指向整型（在此为uint64类型）的指针
//           在这里，ip指向一个uint64类型的变量，用于存储从陷阱帧中获取的地址值
// @note: 这里不检查地址的合法性，因为后续的 `copyin`/`copyout` 会检查。
//
// 示例：
// 对于read(fd, buf, n)系统调用，使用argaddr(1, &buf)获取第二个参数（缓冲区地址）
// 对于write(fd, buf, n)系统调用，使用argaddr(1, &buf)获取第二个参数（缓冲区地址）
// --------------------------------------------------------------------
// [argaddr](#argaddr)
void argaddr(int n, uint64 *ip)
{
  // ip是Integer Pointer的缩写，表示指向整型（在此为uint64类型）的指针
  *ip = argraw(n);
}

// --------------------------------------------------------------------
// argstr
// --------------------------------------------------------------------
// 获取第 n 个字符串类型的系统调用参数。
//
// 此函数首先获取第n个参数作为字符串在用户空间的地址，
// 然后通过fetchstr函数将字符串从用户空间复制到内核空间的缓冲区中。
// 字符串以空字符('\0')结尾，最大复制长度由max参数指定。
//
// @param n: 参数索引（0-5），对应a0-a5寄存器
// @param buf: 内核空间缓冲区，用于存储从用户空间复制的字符串
// @param max: 缓冲区的最大长度，包括结尾的空字符
// @return: 成功则返回字符串长度（不包括'\0'），失败返回-1。
//
// 示例：
// 对于exec(filename, argv)系统调用，使用argstr(0, filename, sizeof(filename))获取第一个参数（文件名）
// 对于open(filename, flags)系统调用，使用argstr(0, filename, sizeof(filename))获取第一个参数（文件名）
// --------------------------------------------------------------------
// [argstr](#argstr)
int argstr(int n, char *buf, int max)
{
  uint64 addr;
  // 获取字符串在用户空间的地址
  argaddr(n, &addr);
  // 从用户空间复制字符串到内核空间
  return fetchstr(addr, buf, max);
}

// 声明在其他文件中（如 sysproc.c, sysfile.c）实现的系统调用处理函数
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);

// ====================================================================
// 系统调用参数传递示例：echo "hi" > x
// ====================================================================
//
// 当用户在shell中执行命令 "echo \"hi\" > x" 时，会发生以下系统调用：
//
// 1. open("x", O_WRONLY|O_CREAT|O_TRUNC, 0664) - 创建/打开文件x用于写入
//    - a7 = SYS_open (15)
//    - a0 = "x"字符串的地址 (第一个参数，文件名)
//    - a1 = O_WRONLY|O_CREAT|O_TRUNC (第二个参数，打开标志)
//    - a2 = 0664 (第三个参数，文件权限)
//    - 返回值：文件描述符（假设为3）
//
// 2. write(1, "hi\n", 3) - 向标准输出写入"hi\n"
//    - a7 = SYS_write (16)
//    - a0 = 1 (第一个参数，标准输出文件描述符)
//    - a1 = "hi\n"字符串的地址 (第二个参数，缓冲区地址)
//    - a2 = 3 (第三个参数，要写入的字节数)
//    - 返回值：实际写入的字节数（3）
//
// 3. dup2(3, 1) - 将文件描述符3复制到1，实现输出重定向
//    - a7 = SYS_dup (10)
//    - a0 = 3 (第一个参数，源文件描述符)
//    - a1 = 1 (第二个参数，目标文件描述符)
//    - 返回值：新的文件描述符（1）
//
// 4. 再次执行write(1, "hi\n", 3) - 现在写入到文件x
//    - a7 = SYS_write (16)
//    - a0 = 1 (第一个参数，文件描述符，现在指向文件x)
//    - a1 = "hi\n"字符串的地址 (第二个参数，缓冲区地址)
//    - a2 = 3 (第三个参数，要写入的字节数)
//    - 返回值：实际写入的字节数（3）
//
// 5. close(1) - 关闭文件描述符1（文件x）
//    - a7 = SYS_close (21)
//    - a0 = 1 (第一个参数，文件描述符)
//    - 返回值：0（成功）
//
// 在这个例子中，a1寄存器在不同系统调用中存储了不同的内容：
// - open系统调用：a1存储文件打开标志(O_WRONLY|O_CREAT|O_TRUNC)
// - write系统调用：a1存储要写入数据的缓冲区地址("hi\n"字符串的地址)
// - dup2系统调用：a1存储目标文件描述符(1)
//
// 这些参数通过argraw(n)函数获取，其中n=1对应a1寄存器的值。
// 例如，在sys_write函数中，通过argaddr(1, &buf)获取a1寄存器的值，
// 即要写入数据的缓冲区地址。

// 系统调用函数指针数组。数组的索引是 `syscall.h` 中定义的系统调用号。
// [sys_calls](#sys_calls)
static uint64 (*syscalls[])(void) = {
    [SYS_fork] sys_fork,
    [SYS_exit] sys_exit,
    [SYS_wait] sys_wait,
    [SYS_pipe] sys_pipe,
    [SYS_read] sys_read,
    [SYS_kill] sys_kill,
    [SYS_exec] sys_exec,
    [SYS_fstat] sys_fstat,
    [SYS_chdir] sys_chdir,
    [SYS_dup] sys_dup,
    [SYS_getpid] sys_getpid,
    [SYS_sbrk] sys_sbrk,
    [SYS_pause] sys_pause,
    [SYS_uptime] sys_uptime,
    [SYS_open] sys_open,
    [SYS_write] sys_write,
    [SYS_mknod] sys_mknod,
    [SYS_unlink] sys_unlink,
    [SYS_link] sys_link,
    [SYS_mkdir] sys_mkdir,
    [SYS_close] sys_close,
};

// --------------------------------------------------------------------
// syscall
// --------------------------------------------------------------------
// 系统调用分发函数。
// 当 `usertrap` 确定陷阱原因是系统调用时（scause == 8），会调用此函数。
//
// 系统调用处理流程：
// 1. 用户程序通过ecall指令触发系统调用
// 2. 硬件保存用户程序上下文，切换到内核模式
// 3. uservec汇编代码保存寄存器到陷阱帧
// 4. usertrap函数识别系统调用并调用本函数
// 5. 本函数根据系统调用号分发到具体的处理函数
// 6. 处理函数通过argraw、argint等辅助函数获取参数
// 7. 处理结果存入a0寄存器，返回给用户程序
//
// 参数传递机制：
// - 系统调用号：存储在a7寄存器中
// - 系统调用参数：依次存储在a0-a5寄存器中（最多6个参数）
// - 系统调用返回值：存储在a0寄存器中返回给用户程序
// --------------------------------------------------------------------
// [syscall](#syscall)
void syscall(void)
{
  int num;
  struct proc *p = myproc();

  // 从 a7 寄存器获取系统调用号
  // a7寄存器在用户空间通过usys.pl生成的汇编代码设置
  // 例如：对于write系统调用，a7被设置为SYS_write（值为16）
  num = p->trapframe->a7;

  // 检查系统调用号是否有效
  if (num > 0 && num < NELEM(syscalls) && syscalls[num])
  {
    // 根据系统调用号，从syscalls数组中找到对应的处理函数并调用。
    // 例如：如果num是SYS_write，则调用sys_write函数

    // 系统调用处理函数内部会通过argraw(n)获取参数：
    // - argraw(0)获取a0寄存器的值（第一个参数）
    // - argraw(1)获取a1寄存器的值（第二个参数）
    // - 以此类推...

    // 将处理函数的返回值存入 a0 寄存器，以便返回给用户程序。
    // 用户程序从系统调用返回后，可以从a0寄存器获取返回值。
    p->trapframe->a0 = syscalls[num]();
  }
  else
  {
    // 无效的系统调用号
    // 打印错误信息，包含进程ID、进程名和无效的系统调用号
    printf("%d %s: unknown sys call %d\n",
           p->pid, p->name, num);

    // 返回-1表示错误
    p->trapframe->a0 = -1;
  }
}