// init: 初始用户级程序 (The initial user-level program)
// 这是xv6系统中第一个运行的进程，负责初始化系统环境并启动shell

#include "kernel/types.h"    // 包含基本数据类型定义
#include "kernel/stat.h"     // 包含文件状态结构体和常量定义
#include "kernel/spinlock.h" // 包含自旋锁结构体和函数声明
#include "kernel/sleeplock.h"// 包含睡眠锁结构体和函数声明
#include "kernel/fs.h"       // 包含文件系统相关结构体和函数声明
#include "kernel/file.h"     // 包含文件描述符和文件操作相关结构体
#include "user/user.h"       // 包含用户程序系统调用接口
#include "kernel/fcntl.h"    // 包含文件控制选项常量定义

// shell程序的参数数组，包含程序名"sh"和空指针结尾
char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;  // pid: 进程ID (Process ID), wpid: 等待的进程ID (Waited Process ID)

  // 尝试打开控制台设备，如果失败则创建并打开
  if(open("console", O_RDWR) < 0){  // O_RDWR: 读写模式 (Read-Write mode)
    mknod("console", CONSOLE, 0);   // 创建控制台设备节点
    open("console", O_RDWR);        // 以读写模式打开控制台
  }
  dup(0);  // 复制文件描述符0作为标准输出 (Standard Output)
  dup(0);  // 复制文件描述符0作为标准错误输出 (Standard Error Output)

  // 无限循环，确保shell始终运行
  for(;;){
    printf("init: starting sh\n");  // 打印启动shell的信息
    pid = fork();                   // 创建子进程 (Create child process)
    if(pid < 0){                    // fork失败
      printf("init: fork failed\n");
      exit(1);                      // 退出程序
    }
    if(pid == 0){                   // 子进程 (Child process)
      exec("sh", argv);             // 执行shell程序 (Execute shell program)
      printf("init: exec sh failed\n");
      exit(1);                      // 如果exec失败则退出
    }

    // 父进程等待子进程结束
    for(;;){
      // wait()调用会在shell退出或无父进程退出时返回
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *) 0);       // 等待任意子进程结束 (Wait for any child process to exit)
      if(wpid == pid){              // shell进程退出
        // the shell exited; restart it.
        break;                      // 跳出内层循环，重新启动shell
      } else if(wpid < 0){          // wait出错
        printf("init: wait returned an error\n");
        exit(1);                    // 退出程序
      } else {                      // 其他无父进程退出
        // it was a parentless process; do nothing.
        // 继续等待shell进程退出
      }
    }
  }
}
