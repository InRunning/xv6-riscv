#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

// 前向声明：加载ELF程序段到页表中
static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// 将ELF段权限标志转换为页表项(PTE)权限位
// ELF权限标志：
//   0x1: 可执行 (PF_X)
//   0x2: 可写 (PF_W)
//   0x4: 可读 (PF_R)
// 页表权限位：
//   PTE_X: 可执行
//   PTE_W: 可写
//   PTE_R: 可读 (默认包含)
int flags2perm(int flags)
{
    int perm = 0;
    // 如果ELF段有可执行权限，设置PTE_X位
    if(flags & 0x1)
      perm = PTE_X;
    // 如果ELF段有可写权限，设置PTE_W位
    if(flags & 0x2)
      perm |= PTE_W;
    // 注意：可读权限(PTE_R)默认包含，不需要显式设置
    return perm;
}

//
// exec()系统调用的实现
// 功能：用一个新的程序替换当前进程的内存映像
// 参数：
//   path: 可执行文件的路径
//   argv: 命令行参数数组
// 返回值：成功返回参数个数argc，失败返回-1
//
int
kexec(char *path, char **argv)
{
  char *s, *last;         // 用于提取文件名的字符串指针
  int i, off;             // 迭代索引与文件偏移量
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;  // argc(Argument Count 参数个数)、sz(Size 进程内存大小)、sp(Stack Pointer 栈顶指针)、ustack用户栈(User Stack 用户栈)地址数组、stackbase栈基址
  struct elfhdr elf;      // ELF (Executable and Linkable Format 可执行与可链接格式) 头部结构体
  struct inode *ip;       // inode(Index Node 索引节点) 指针，用于文件操作
  struct proghdr ph;      // ELF (Executable and Linkable Format 可执行与可链接格式) Program Header 程序头结构体
  pagetable_t pagetable = 0, oldpagetable;  // 新旧页表指针
  struct proc *p = myproc();  // 指向当前进程的指针

  // 开始文件系统操作
  begin_op();

  // 打开可执行文件
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  // 锁定inode，防止其他进程修改
  ilock(ip);

  // 读取ELF文件头
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // 检查是否是有效的ELF文件
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // 为进程创建新的页表
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // 加载程序到内存
  // 遍历所有ELF程序段
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    // 读取程序头
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    // 只处理需要加载的段(PT_LOAD, Program Header Type Load 可加载类型)
    if(ph.type != ELF_PROG_LOAD)
      continue;
    // 检查内存大小不小于文件大小
    if(ph.memsz < ph.filesz)
      goto bad;
    // 检查虚拟地址不溢出
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    // 检查虚拟地址页对齐
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    // 为该段分配虚拟内存空间
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    // 从文件加载段内容到内存
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  // 释放文件inode并结束文件系统操作
  iunlockput(ip);
  end_op();
  ip = 0;

  // 重新获取当前进程指针(可能在过程中发生变化)
  p = myproc();
  uint64 oldsz = p->sz;

  // 在下一个页面边界分配一些页面作为用户栈
  // 第一个页面设为不可访问，作为栈保护页
  // 其余页面用作用户栈
  sz = PGROUNDUP(sz);
  uint64 sz1;
  // 分配用户栈空间，包括一个保护页
  if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  // 清除保护页的映射，使其不可访问
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;  // 栈顶指针
  stackbase = sp - USERSTACK*PGSIZE;  // 栈基址

  // 将参数字符串复制到新栈中，并在ustack[]中记住它们的地址
  for(argc = 0; argv[argc]; argc++) {
    // 检查参数数量是否超过最大值
    if(argc >= MAXARG)
      goto bad;
    // 为参数字符串分配空间(包括结尾的null字符)
    sp -= strlen(argv[argc]) + 1;
    // RISC-V要求栈指针16字节对齐
    sp -= sp % 16;
    // 检查栈是否溢出
    if(sp < stackbase)
      goto bad;
    // 将参数字符串复制到用户栈
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    // 保存参数字符串在栈中的地址
    ustack[argc] = sp;
  }
  // argv(Argument Vector 参数向量) 数组以NULL结尾
  ustack[argc] = 0;

  // 将ustack[]数组(argv, Argument Vector 参数向量 指针数组)压入栈中
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;  // 保持16字节对齐
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0和a1包含传递给用户main(argc, Argument Count 参数个数; argv, Argument Vector 参数向量)的参数
  // argc(Argument Count 参数个数) 通过系统调用返回值传递，它放在a0寄存器中
  // argv(Argument Vector 参数向量) 指针数组的地址放在a1寄存器中
  p->trapframe->a1 = sp;

  // 保存程序名用于调试
  // 提取路径中的文件名部分
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // 提交到用户映像
  // 保存旧页表，以便在失败时恢复
  oldpagetable = p->pagetable;
  // 设置新页表
  p->pagetable = pagetable;
  // 更新进程大小
  p->sz = sz;
  // 设置程序计数器为ELF入口点(通常是ulib.c中的start()函数)
  p->trapframe->epc = elf.entry;
  // 设置栈指针
  p->trapframe->sp = sp;
  // 释放旧页表
  proc_freepagetable(oldpagetable, oldsz);

  // 返回参数个数argc(Argument Count 参数个数)，这个值最终会放在a0寄存器中，
  // 作为main(argc, Argument Count 参数个数; argv, Argument Vector 参数向量)的第一个参数
  return argc;

 bad:
  // 错误处理：清理已分配的资源
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// 将ELF程序段加载到页表中的虚拟地址va处
// 参数：
//   pagetable: 目标页表
//   va: 虚拟地址(必须页对齐)
//   ip: 文件inode(Index Node 索引节点)
//   offset: 文件中的偏移量
//   sz: 段大小
// 要求：
//   va必须页对齐
//   从va到va+sz的页面必须已经映射
// 返回值：成功返回0，失败返回-1
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;  // 物理地址

  // 逐页加载段内容
  for(i = 0; i < sz; i += PGSIZE){
    // 获取虚拟地址对应的物理地址
    pa = walkaddr(pagetable, va + i);
    // 如果地址不存在，说明页表设置有问题
    if(pa == 0)
      panic("loadseg: address should exist");
    // 计算本次要读取的字节数
    if(sz - i < PGSIZE)
      n = sz - i;  // 剩余不足一页
    else
      n = PGSIZE;  // 一整页
    // 从文件读取数据到物理内存
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
