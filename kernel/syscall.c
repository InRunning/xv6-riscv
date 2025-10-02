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
// @return: 成功返回0，失败返回-1。
// --------------------------------------------------------------------
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // 检查地址是否在进程的合法范围内
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
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
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

// --------------------------------------------------------------------
// argraw
// --------------------------------------------------------------------
// 从陷阱帧中获取第 n 个原始的、未经处理的64位系统调用参数。
// @param n: 参数的索引 (0-5)。
// @return: 参数的值。
// --------------------------------------------------------------------
static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// --------------------------------------------------------------------
// argint
// --------------------------------------------------------------------
// 获取第 n 个32位整型系统调用参数。
// --------------------------------------------------------------------
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// --------------------------------------------------------------------
// argaddr
// --------------------------------------------------------------------
// 获取第 n 个指针类型的系统调用参数。
// @note: 这里不检查地址的合法性，因为后续的 `copyin`/`copyout` 会检查。
// --------------------------------------------------------------------
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// --------------------------------------------------------------------
// argstr
// --------------------------------------------------------------------
// 获取第 n 个字符串类型的系统调用参数。
// @return: 成功则返回字符串长度，失败返回-1。
// --------------------------------------------------------------------
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
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

// 系统调用函数指针数组。数组的索引是 `syscall.h` 中定义的系统调用号。
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_pause]   sys_pause,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
};

// --------------------------------------------------------------------
// syscall
// --------------------------------------------------------------------
// 系统调用分发函数。
// 当 `usertrap` 确定陷阱原因是系统调用时，会调用此函数。
// --------------------------------------------------------------------
void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  // 从 a7 寄存器获取系统调用号
  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // 根据系统调用号，从数组中找到对应的处理函数并调用。
    // 将返回值存入 a0 寄存器，以便返回给用户程序。
    p->trapframe->a0 = syscalls[num]();
  } else {
    // 无效的系统调用号
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}