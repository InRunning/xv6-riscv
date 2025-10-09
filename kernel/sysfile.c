//
// 文件系统相关的系统调用实现。
// 主要负责参数校验（我们不信任用户态传入的数据），然后再调用 file.c 与 fs.c 中的底层函数。
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// 将系统调用的第 n 个参数解析为文件描述符（FD, file descriptor 文件描述符），
// 并返回描述符本身以及对应的 struct file 指针。
// 这个辅助函数避免其他系统调用直接访问用户内存：负责范围检查、确认该描述符有效，
// 并在需要时通过 `pfd` 返回原始整数，通过 `pf` 返回 struct file 指针。
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// 为给定的 struct file 分配一个文件描述符。
// 成功时接手调用者的文件引用（即调用者不再需要额外持有它）。
// 返回的整数始终是该进程描述符表中最小的未占用槽位，
// 遵循 Unix“最小可用文件描述符”约定，这也是 shell 重定向能够生效的关键。
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  // 复制已有的文件描述符，使新描述符引用同一个底层打开文件（Open File Description）。
  // 执行步骤：
  // 1. `argfd` 解析第 0 个参数，得到源文件描述符。
  // 2. `fdalloc` 找到一个空槽并放入同一个 struct file 指针。
  // 3. `filedup` 增加文件层维护的引用计数。
  // 最终结果：两个整数描述符共享同一份状态（偏移量、访问模式等）。
  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  // read 系统调用入口。
  // - `argaddr` 取得用户缓冲区指针（参数 1）。
  // - `argint` 获取请求的字节数（参数 2）。
  // - `argfd` 将参数 0 的文件描述符解析成 struct file。
  // 随后交给 `fileread`，由它处理管道、设备和 inode 等不同类型。
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  // write 系统调用的核心处理逻辑：从陷入帧（Trap Frame）拷出参数，
  // 转换成内核可以理解的对象，然后把实际写入交给更高层的文件子系统。
  //
  // 处理步骤：
  // 1. `argaddr`（argument address 参数地址）取得用户空间缓冲区指针。
  // 2. `argint`（argument integer 参数整数）取得期望写入的字节数。
  // 3. `argfd`（argument file descriptor 参数文件描述符）解析用户给出的
  //    文件描述符（FD, file descriptor 文件描述符），并验证访问权限。
  // 4. `filewrite` 根据文件类型执行真正的写入，inode 类型还会结合日志（Log）。
  //
  // 返回值：
  // - 成功时返回写入的字节数。
  // - 参数解析失败或底层写入出错时返回 -1。
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  // close 系统调用：关闭一个文件描述符。
  // `argfd` 同时取回整数描述符和 struct file 指针；
  // 将 `ofile[fd]` 置空即可把该描述符从进程表中移除，
  // 随后 `fileclose` 递减共享引用计数，必要时释放底层资源。
  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  // 将打开文件的元数据填充到用户提供的 `struct stat`（status 状态）结构中。
  // `filestat` 会把设备号、inode 类型、文件大小、链接计数等信息拷贝到 `st` 指向的用户缓冲区。
  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// 创建路径 `new`，使其指向与 `old` 相同的 inode。
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  // 创建一个硬链接（Hard Link），让路径 `new` 指向与 `old` 相同的 inode。
  // 操作流程：
  // 1. 从用户空间复制两个路径字符串。
  // 2. 调用 `begin_op` 开启文件系统日志事务。
  // 3. 通过 `namei(old)` 找到已有的 inode，确认它不是目录（目录的硬链接会破坏层次结构）。
  // 4. 递增该 inode 的链接计数 `nlink` 并写回，这样即便后续失败，旧引用也不会丢失。
  // 5. 使用 `nameiparent` 找到 `new` 的父目录，在其中插入指向同一 inode 编号的目录项。
  // 6. 若过程中任何一步失败，回滚 `nlink` 并结束事务。
  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op(); // 开启日志事务，确保后续修改受日志保护
  if((ip = namei(old)) == 0){ // 查找旧路径对应的 inode
    end_op(); // 找不到目标，直接结束事务
    return -1;
  }

  ilock(ip); // 给目标 inode 加锁，防止并发修改
  if(ip->type == T_DIR){ // 禁止对目录创建硬链接
    iunlockput(ip); // 释放锁并丢弃引用
    end_op();       // 结束事务
    return -1;
  }

  ip->nlink++; // 增加链接计数，保证 inode 不会被提前回收
  iupdate(ip); // 将新的链接计数写回磁盘
  iunlock(ip); // 释放 inode 锁

  if((dp = nameiparent(new, name)) == 0) // 查找新路径的父目录
    goto bad;
  ilock(dp); // 锁定父目录
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){ // 设备号必须一致，并插入目录项
    iunlockput(dp); // 失败时释放父目录
    goto bad;
  }
  iunlockput(dp); // 释放父目录锁和引用
  iput(ip);       // 释放目标 inode 引用

  end_op();       // 正常结束事务

  return 0;

bad:
  ilock(ip);    // 发生错误，需要回滚链接计数
  ip->nlink--;  // 恢复到原始值
  iupdate(ip);  // 写回磁盘
  iunlockput(ip); // 释放锁并减少引用
  end_op();        // 结束事务
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  // 遍历目录 inode，检查除 "." 与 ".." 以外的目录项是否全部为空。
  // 只要发现非空项就返回 0；全部为空则返回 1。
  // 通过 `readi`（read inode 读取 inode）按固定大小读取目录项。
  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){ // 跳过前两个目录项 "." 和 ".."
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) // 逐项读取目录项
      panic("isdirempty: readi");
    if(de.inum != 0) // 只要找到非空条目，就说明目录不为空
      return 0;
  }
  return 1; // 全部扫描完毕都为空，返回 1
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  // 删除目录项，同时减少目标 inode 的链接计数。
  // 关键约束：
  // - 不能删除 "." 或 ".."。
  // - 如果目标是目录，必须为空（通过 `isdirempty` 检查）。
  // 执行路径：
  // 1. 使用 `nameiparent` 获取父目录并加锁。
  // 2. 通过 `dirlookup` 找到目录项，对应的 inode 加锁并检查状态。
  // 3. 调用 `writei`（write inode 写 inode）将该目录项清零。
  // 4. 调整链接计数：若删除的是子目录，父目录的 `nlink` 也要减 1；目标 inode 的 `nlink` 一定要减 1。
  // 5. 结束日志事务 `end_op`，依次释放所有锁。
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp); // 给父目录加锁，准备修改目录项

  // 不能删除 "." 或 ".." 这两个保留目录项。
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0) // 查找待删除的 inode，并记录目录项偏移
    goto bad;
  ilock(ip); // 锁定目标 inode

  if(ip->nlink < 1) // 链接计数异常，说明内部状态损坏
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){ // 目录必须为空才能删除
    iunlockput(ip); // 释放目录 inode
    goto bad;       // 回到错误处理
  }

  memset(&de, 0, sizeof(de)); // 构造空目录项
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) // 写回目录，清除目录项
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;  // 删除目录时，父目录对 ".." 的链接也要减少
    iupdate(dp);  // 更新父目录的链接计数
  }
  iunlockput(dp); // 释放父目录锁和引用

  ip->nlink--;  // 目标 inode 链接计数减一
  iupdate(ip);  // 写回磁盘
  iunlockput(ip); // 释放 inode 锁和引用

  end_op(); // 日志事务结束

  return 0;

bad:
  iunlockput(dp); // 出错时释放父目录
  end_op();       // 结束事务
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  // 内部辅助函数：为普通文件、目录或设备节点创建（或复用）一个 inode。
  // 参数说明：
  // - `type`：取值为 `T_FILE`、`T_DIR`、`T_DEVICE` 之一。
  // - `major` / `minor`：用于 `T_DEVICE` 的主从设备号。
  //
  // 算法概述：
  // 1. 使用 `nameiparent` 找到父目录。
  // 2. 如果目标名称已存在：
  //    - 若请求类型是普通文件，且已有 inode 类型为 `T_FILE` 或 `T_DEVICE`，则直接返回该 inode（供 `open` 复用）。
  //    - 否则视为冲突，返回空指针。
  // 3. 通过 `ialloc` 分配新 inode，初始化元数据并用 `iupdate` 写回磁盘。
  // 4. 若创建目录，额外写入 "." 与 ".." 目录项。
  // 5. 调用 `dirlink` 将新名称插入父目录，并在创建目录时增加父目录的 `nlink` 计数（因为多了一个 ".." 指向它）。
  // 6. 出错时撤销已分配的资源并返回 0。
  if((dp = nameiparent(path, name)) == 0) // 找到父目录，并把末级名称写入 name
    return 0;

  ilock(dp); // 锁住父目录，准备查询或写入目录项

  if((ip = dirlookup(dp, name, 0)) != 0){ // 目标已存在
    iunlockput(dp); // 释放父目录
    ilock(ip);      // 锁定已有 inode
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE)) // 普通文件允许复用
      return ip;
    iunlockput(ip); // 类型不匹配，释放并返回失败
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){ // 分配新 inode
    iunlockput(dp);
    return 0;
  }

  ilock(ip);           // 锁定新 inode
  ip->major = major;   // 设备号初始化
  ip->minor = minor;
  ip->nlink = 1;       // 新 inode 默认链接计数为 1
  iupdate(ip);         // 写回 inode 元数据

  if(type == T_DIR){ // 为目录写入 "." 与 ".." 项
    // 不对 "." 增加链接计数，避免形成循环计数。
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0) // 将新 inode 链接到父目录
    goto fail;

  if(type == T_DIR){
    // 目录已成功插入，此时需要增加父目录对 ".." 的引用。
    dp->nlink++;
    iupdate(dp);
  }

  iunlockput(dp); // 释放父目录

  return ip;

 fail:
  // 出错时撤销新 inode：清零链接计数并更新磁盘，然后释放锁和父目录。
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  // open 系统调用支撑了 shell 的重定向流程：解析用户给出的路径，必要时新建 inode，
  // 并把新的文件描述符（FD, file descriptor 文件描述符）注册到当前进程。
  //
  // 缩写说明：
  // - `omode`：open mode 打开模式掩码，例如 `O_CREATE`、`O_RDWR`、`O_TRUNC`（truncate 截断）。
  //
  // 执行流程：
  // 1. 从陷入帧复制路径字符串和模式位。
  // 2. 调用 `begin_op` 开启日志事务，以便后续 inode 分配写入日志。
  // 3. 如果设置了 `O_CREATE`（open create 打开并创建），调用 `create` 分配新 inode；
  //    否则通过 `namei`（name inode 路径解析）找到已有 inode。
  // 4. 给 inode 加锁，拒绝不符合访问模式的目录打开请求，接着用 `filealloc` 创建 struct file。
  // 5. 把 struct file 交给 `fdalloc`（file descriptor allocate 分配文件描述符），获得最小可用的描述符编号。
  // 6. 任一步失败都需要回滚已分配资源并返回 -1。

  argint(1, &omode); // 获取第二个参数：打开模式
  if((n = argstr(0, path, MAXPATH)) < 0) // 拷贝第一个参数：路径字符串
    return -1;

  begin_op(); // 开启日志事务

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0); // 需要创建新文件
    if(ip == 0){ // 创建失败
      end_op(); // 结束事务
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){ // 查找已有文件
      end_op(); // 找不到则返回错误
      return -1;
    }
    ilock(ip); // 锁定 inode，检查类型
    if(ip->type == T_DIR && omode != O_RDONLY){ // 目录只允许只读打开
      iunlockput(ip); // 释放 inode
      end_op();       // 结束事务
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){ // 设备号范围检查
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){ // 分配 struct file 与文件描述符
    if(f)
      fileclose(f); // 如果只成功了一半，需要收尾
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE; // 设备文件：记录主设备号
    f->major = ip->major;
  } else {
    f->type = FD_INODE; // 普通文件：重置文件偏移
    f->off = 0;
  }
  f->ip = ip; // 记录 inode 指针
  f->readable = !(omode & O_WRONLY); // 设置可读标志
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR); // 设置可写标志

  if((omode & O_TRUNC) && ip->type == T_FILE){ // 需要截断文件内容
    itrunc(ip);
  }

  iunlock(ip); // 解锁 inode
  end_op();    // 结束事务

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  // mkdir 系统调用：创建一个目录。
  // `create` 会负责分配 inode 并写入 "." / ".." 等目录项。
  begin_op(); // 开启事务
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op(); // 参数错误或创建失败
    return -1;
  }
  iunlockput(ip); // 释放新目录的锁与引用
  end_op();       // 提交事务
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  // mknod 系统调用：创建设备特殊文件。
  // 会生成一个 `T_DEVICE` 类型的 inode，并记录主/次设备号供字符设备驱动使用。
  begin_op();          // 开启事务
  argint(1, &major);  // 读取主设备号
  argint(2, &minor);  // 读取次设备号
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op(); // 失败时结束事务
    return -1;
  }
  iunlockput(ip); // 释放新建设备节点
  end_op();       // 提交事务
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  // chdir 系统调用：切换当前工作目录（CWD）。
  // 流程：
  // 1. 查找目标路径对应的 inode。
  // 2. 确认它是目录类型。
  // 3. 释放旧的工作目录引用，把新 inode 赋给进程。
  // `iput` 只会减少引用计数，不会强制释放仍被其他描述符引用的目录。
  begin_op(); // 开启事务，防止查找过程中 inode 被释放
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op(); // 路径错误
    return -1;
  }
  ilock(ip); // 锁定目标 inode
  if(ip->type != T_DIR){
    iunlockput(ip); // 不是目录则失败
    end_op();
    return -1;
  }
  iunlock(ip);   // 释放目标目录锁
  iput(p->cwd);  // 释放旧的工作目录
  end_op();      // 提交事务
  p->cwd = ip;   // 替换为新的工作目录
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG]; // `path` 缓冲路径字符串，`argv` 保存内核态参数指针数组
  int i;                             // 遍历参数时的索引
  uint64 uargv, uarg;                // `uargv` 指向用户态 argv 数组，`uarg` 是单个用户态参数指针

  // exec 系统调用：用一个新的用户程序替换当前进程的内存映像。
  // 这个包装函数先把用户态的指针转换为内核缓冲区，再调用 `kexec`（kernel exec 内核级 exec）。
  //
  // 详细步骤：
  // 1. 获取参数向量指针 `uargv`，并把可执行文件路径复制到内核缓冲区。
  // 2. 遍历参数向量，把每个字符串拷贝到内核分配的页内存，避免在切换页表时发生缺页。
  // 3. 调用 `kexec`，加载 ELF（Executable and Linkable Format 可执行与可链接格式）文件，
  //    构建新的页表，并搭建用户栈。
  // 4. 无论成功或失败，都要释放临时分配的参数缓冲区。
  //
  // 返回值含义：
  // - 成功时返回新程序的参数个数；由于陷入帧被替换，实际不会返回到旧的用户态。
  // - 失败时返回 -1。

  argaddr(1, &uargv); // 参数 1：用户空间的 argv 指针数组
  if(argstr(0, path, MAXPATH) < 0) { // 参数 0：可执行文件路径
    return -1;
  }
  memset(argv, 0, sizeof(argv)); // 内核临时数组置空
  for(i=0;; i++){
    if(i >= NELEM(argv)){ // 参数数目超过限制
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){ // 读取单个参数指针
      goto bad;
    }
    if(uarg == 0){ // 遇到结尾 NULL
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc(); // 为参数字符串分配一页
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0) // 把用户字符串拷贝到内核缓冲区
      goto bad;
  }

  int ret = kexec(path, argv); // 加载并执行新程序

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]); // 释放临时参数内存

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]); // 清理已分配的缓冲区
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // 用户态两个整数的数组指针
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  // pipe 系统调用：创建单向管道。
  // - `pipealloc` 返回读端（`rf`）和写端（`wf`）两个 struct file。
  // - `fdalloc` 将它们塞进当前进程的文件描述符表，通常得到一对相邻的描述符。
  // - `copyout` 把这两个整数写回用户缓冲区，供用户程序使用。
  // 如果中途失败，需要仔细回滚：关闭已分配的描述符并释放 struct file。
  argaddr(0, &fdarray); // 参数 0：用户传入的存放结果的指针
  if(pipealloc(&rf, &wf) < 0) // 分配管道端点
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){ // 为读端/写端分配描述符
    if(fd0 >= 0)
      p->ofile[fd0] = 0; // 撤销已经登记的描述符
    fileclose(rf); // 释放 struct file
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 || // 写回读端 fd
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){ // 写回写端 fd
    p->ofile[fd0] = 0; // 出错则清理
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
