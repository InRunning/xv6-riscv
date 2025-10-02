//
// 控制台的输入和输出，底层连接到 UART (通用异步收发传输器)。
// 读取操作是按行进行的。
// 实现了一些特殊的控制字符输入:
//   newline (\n) -- 行结束
//   control-h    -- 退格
//   control-u    -- 删除整行
//   control-d    -- 文件结束 (EOF)
//   control-p    -- 打印进程列表
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

// 1. 宏定义与全局状态 ----

#define BACKSPACE 0x100      // 定义退格键的特殊编码
#define C(x)  ((x)-'@')      // Control-x 组合键的宏

// `cons` 结构体保存了控制台输入缓冲区的全部状态。
// 它由一个自旋锁 `lock` 保护，以防止多核竞争。
struct {
  struct spinlock lock;
  
  // 输入缓冲区是一个环形缓冲区 (circular buffer)。
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // 读取指针 (Read index): consoleread() 将要读取的下一个字符的位置。
  uint w;  // 写入指针 (Write index): consoleintr() 将要写入的下一个字符的位置。
  uint e;  // 编辑指针 (Edit index): 当前正在编辑的行的末尾位置。
} cons;

// 2. 控制台输出 ----

//
// 发送单个字符到 UART。
// 这是一个底层的输出函数。
// 它被 printf() (用于内核消息) 和用于回显输入字符时调用。
// 它不会被 write() 系统调用直接调用。
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // 如果字符是退格键，发送 "退格-空格-退格" 序列：
    // '\b' (光标左移), ' ' (用空格覆盖), '\b' (光标再次左移)。
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    // 否则，直接将字符发送给 UART 硬件。
    uartputc_sync(c);
  }
}

//
// 处理用户态对控制台设备的 write() 系统调用。
// 它将数据从用户空间拷贝到内核的一个临时缓冲区，然后发送给 UART。
//
int
consolewrite(int user_src, uint64 src, int n)
{
  int i = 0;

  // 以小块 (chunk) 的方式循环处理用户数据。
  while(i < n){
    char buf; // 一个小型的内核临时缓冲区。
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i; // 确保不会读取超过用户提供的数据末尾。

    // 从用户空间 (或内核空间) 拷贝一小块数据到内核缓冲区 `buf`。
    if(either_copyin(buf, user_src, src + i, nn) == -1)
      break; // 如果拷贝出错则停止。

    // 将内核缓冲区中的数据块写入 UART。
    uartwrite(buf, nn);
    
    i += nn;
  }

  return i; // 返回成功写入的字节数。
}

// 3. 控制台输入 ----

//
// 处理用户态从控制台设备的 read() 系统调用。
// 它会一直等待，直到有一整行输入可用，然后将其拷贝给用户进程。
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n; // 记录用户请求的原始长度。
  acquire(&cons.lock);

  while(n > 0){
    // 等待，直到中断处理程序向缓冲区中放入了一整行数据。
    // 一行完整的标志是 `cons.w` 被更新，不再等于 `cons.r`。
    while(cons.r == cons.w){
      if(killed(myproc())){
        // 如果当前进程被杀死了，则中止读取操作。
        release(&cons.lock);
        return -1;
      }
      // 进入睡眠状态，等待中断处理程序唤醒。
      // 在睡眠前，`sleep` 函数会原子地释放锁；唤醒后会重新获取锁。
      sleep(&cons.r, &cons.lock);
    }

    // 从环形缓冲区中读取一个字符。
    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // Control-D 表示文件结束 (EOF)。
      if(n < target){
        // 如果已经拷贝了一些字符给用户，那么暂时不消耗这个 ^D。
        // 把它留给下一次 read() 调用，让它读到并返回 0 字节。
        cons.r--;
      }
      break; // 停止读取。
    }

    // 将读取到的字符拷贝到目标缓冲区 (用户空间或内核空间)。
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break; // 如果拷贝出错则停止。

    dst++;
    --n;

    if(c == '\n'){
      // 如果读到了换行符，说明一整行已经处理完毕，停止读取。
      break;
    }
  }
  release(&cons.lock);

  return target - n; // 返回成功读取的字节数。
}

// 4. 中断处理程序 ----

//
// 控制台输入的中断处理程序。
// 当 UART 硬件接收到每个字符时，`uartintr()` 会调用这个函数。
// 它负责处理特殊字符 (如退格、删除行)、将字符回显到屏幕上，
// 并在接收到一整行时唤醒任何正在休眠的 `consoleread()` 进程。
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Control-P: 打印进程列表，用于调试。
    procdump();
    break;

  case C('U'):  // Control-U: 删除行。删除当前输入行中的所有字符。
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;

  case C('H'): // Control-H: 退格键。
  case '\x7f': // Delete 键也视为退格。
    if(cons.e != cons.w){
      cons.e--; // 将编辑指针向后移动。
      consputc(BACKSPACE); // 在屏幕上视觉地擦除一个字符。
    }
    break;

  default:
    // 对于任何其他字符，将其添加到输入缓冲区。
    if(c != 0 && cons.e - cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c; // 将回车符 ('\r') 转换成换行符 ('\n')。

      // 将字符回显给用户的屏幕。
      consputc(c);

      // 将字符存入环形缓冲区的当前编辑位置。
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e - cons.r == INPUT_BUF_SIZE){
        // 当用户输入换行、^D 或缓冲区满时，认为一行输入完成。
        // 更新 `w` 指针，使这一行对 `consoleread()` 可见。
        cons.w = cons.e;
        // 唤醒任何在 `cons.r` 上睡眠的进程。
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

// 5. 初始化 ----

//
// consoleinit(void) - 初始化控制台设备
//
// @description:
//   此函数在内核启动时被调用，用于完成控制台设备的全部设置工作。
//   它负责初始化保护控制台数据结构的锁，设置底层的 UART 硬件，
//   并将控制台的读写功能注册到内核的设备管理体系中，从而让用户进程
//   可以通过标准的文件描述符（如 0, 1, 2）与控制台进行交互。
//
// @context:
//   由 CPU 0 在 kernel/main.c 中的 main() 函数调用，是系统早期初始化的一部分。
//
void
consoleinit(void)
{
  // 1. 初始化自旋锁:
  //    `initlock` 函数初始化 `cons` 全局结构体中的自旋锁。
  //    这个锁至关重要，因为它保护着 `cons.buf` 输入缓冲区，防止
  //    在多核环境下，当中断处理程序 (consoleintr) 尝试写入缓冲区时，
  //    另一个核心上的 `consoleread` 正在读取缓冲区，从而导致数据竞争和状态不一致。
  //    "cons" 是锁的调试名称，如果系统因死锁而崩溃，这个名称会显示出来。
  initlock(&cons.lock, "cons");

  // 2. 初始化 UART 硬件:
  //    `uartinit` 是一个底层函数 (定义在 kernel/uart.c)，它负责
  //    配置物理的 UART 芯片。这包括设置波特率等通信参数，并为 UART 开启中断，
  //    这样每当有新的输入字符到达时，硬件就能通知 CPU，进而触发中断处理。
  uartinit();

  // 3. 注册设备驱动到 devsw (device switch) 表:
  //    `devsw` 是一个全局的“设备驱动表”，它是一个函数指针数组。
  //    内核通过这个表将通用的文件操作 (read, write) 映射到具体的设备驱动函数。
  //    这里的代码将 `devsw` 数组中索引为 `CONSOLE` 的条目的 `read` 和 `write`
  //    函数指针，分别指向我们在这个文件中实现的 `consoleread` 和 `consolewrite` 函数。
  //    这样，当用户进程对控制台文件描述符执行 read() 或 write() 系统调用时，
  //    内核就会通过此表查找到并调用这两个函数。
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}