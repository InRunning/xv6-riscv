// 包含内核类型定义（Kernel Types Definition）头文件
#include "kernel/types.h"
// 包含文件状态（File Status）结构定义头文件
#include "kernel/stat.h"
// 包含文件控制（File Control）标志定义头文件
#include "kernel/fcntl.h"
// 包含RISC-V架构（Reduced Instruction Set Computer V）相关定义头文件
#include "kernel/riscv.h"
// 包含虚拟内存管理（Virtual Memory）相关定义头文件
#include "kernel/vm.h"
// 包含用户空间（User Space）函数声明头文件
#include "user/user.h"

//
// 包装函数（Wrapper Function），确保即使main()函数不调用exit()也能正常退出
//
void
start(int argc, char **argv)
{
  int r;  // 用于存储main函数的返回值（Return value）
  extern int main(int argc, char **argv);  // 声明main函数为外部函数（External function）
  r = main(argc, argv);  // 调用main函数并获取返回值
  exit(r);  // 使用main函数的返回值作为退出码（Exit code）退出程序
}

// 字符串复制（String Copy）函数
// 将源字符串t复制到目标字符串s中
char*
strcpy(char *s, const char *t)
{
  char *os;  // 保存目标字符串的起始地址（Original string）

  os = s;  // 记录目标字符串的起始位置
  // 循环复制字符，直到遇到空字符（Null character）('\0')
  // 先赋值*s = *t，然后s和t都递增（Increment），最后检查赋值结果是否为0
  while((*s++ = *t++) != 0)
    ;  // 空循环体，所有工作都在循环条件中完成
  return os;  // 返回目标字符串的起始地址
}

// 字符串比较（String Compare）函数
// 比较两个字符串是否相等，返回比较结果
int
strcmp(const char *p, const char *q)
{
  // 当p指向的字符不为空且p和q指向的字符相等时，继续比较
  while(*p && *p == *q)
    p++, q++;  // 递增（Increment）指针，继续比较下一个字符
  // 返回两个字符的差值：
  // 如果相等，返回0
  // 如果p的字符ASCII码（American Standard Code for Information Interchange）大于q的字符ASCII码，返回正值
  // 如果p的字符ASCII码小于q的字符ASCII码，返回负值
  return (uchar)*p - (uchar)*q;
}

// 计算字符串长度（String Length）的函数
//
// 函数功能：
// 计算以空字符（Null character）('\0')结尾的字符串的长度，不包括结尾的空字符。
// 这是C标准库（C Standard Library）中的一个基本函数，用于确定字符串中字符的数量。
//
// 参数说明：
// s - 指向以空字符结尾的字符串的常量字符指针（Constant character pointer）
//    const修饰符（Constant modifier）表示函数不会修改传入的字符串内容
//
// 返回值：
// 返回一个无符号整数（Unsigned Integer），表示字符串中的字符数量（不包括结尾的空字符）
//
// 实现原理：
// 1. 初始化计数器n为0
// 2. 使用for循环遍历字符串，直到遇到空字符（Null character）('\0')为止
// 3. 每次循环检查s[n]是否为非零值（非空字符）
// 4. 如果是非空字符，计数器n递增（Increment）
// 5. 当遇到空字符时，循环结束，返回计数器n的值
//
// 注意事项：
// - 调用者必须确保传入的字符串是以空字符结尾的，否则会导致未定义行为（Undefined behavior）
// - 函数不会检查指针s是否为NULL（Null pointer），如果传入NULL指针会导致程序崩溃
// - 返回值类型是uint（Unsigned Integer），可以处理很长的字符串
//
// 使用示例：
// char *str = "Hello";
// uint len = strlen(str);  // len的值为5
uint
strlen(const char *s)
{
  int n;  // 计数器（Counter），用于记录字符串长度

  // 遍历字符串，直到遇到空字符（Null character）('\0')
  // s[n]会在遇到空字符时返回0，循环条件为假，循环结束
  for(n = 0; s[n]; n++)
    ;  // 空循环体，所有工作都在循环条件中完成
  return n;  // 返回字符串长度
}

// 内存设置（Memory Set）函数
// 将内存区域的前n个字节设置为指定的字符c
void*
memset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;  // 将void指针转换为char指针以便字节级操作
  int i;  // 循环计数器（Loop counter）
  // 循环n次，将每个字节都设置为字符c
  for(i = 0; i < n; i++){
    cdst[i] = c;  // 将当前字节设置为字符c
  }
  return dst;  // 返回目标内存区域的起始地址
}

// 字符查找（String Character）函数
// 在字符串s中查找字符c，如果找到返回字符的指针，否则返回NULL（Null pointer）
char*
strchr(const char *s, char c)
{
  // 遍历字符串，直到遇到空字符（Null character）('\0')
  for(; *s; s++)
    // 如果当前字符等于要查找的字符c
    if(*s == c)
      return (char*)s;  // 返回当前字符的指针
  return 0;  // 如果没有找到字符c，返回NULL
}

// 字符串输入（String Get）函数
// 从标准输入（Standard Input）读取一行字符到缓冲区buf中
char*
gets(char *buf, int max)
{
  int i, cc;  // i: 缓冲区索引（Buffer index）, cc: 读取的字节数（Bytes count）
  char c;  // 存储读取的字符（Character）

  // 循环读取字符，直到缓冲区满或遇到换行符（Newline）
  // i+1 < max 确保至少保留一个字节给字符串结束符（Null terminator）'\0'
  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);  // 从标准输入(Standard Input)(文件描述符0)读取1个字符
    if(cc < 1)  // 如果读取失败或到达文件末尾（End of File）
      break;  // 退出循环
    buf[i++] = c;  // 将字符存入缓冲区，并递增（Increment）索引
    // 如果遇到换行符（Newline）('\n')或回车符（Carriage return）('\r')，结束输入
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';  // 在字符串末尾添加空字符（Null character），确保字符串正确终止
  return buf;  // 返回缓冲区指针
}

// 文件状态获取（File Status）函数
// 获取指定文件的状态信息并存储在stat结构体（Status structure）中
int
stat(const char *n, struct stat *st)
{
  int fd;  // 文件描述符（File descriptor）
  int r;  // 函数返回值（Return value）

  fd = open(n, O_RDONLY);  // 以只读（Read Only）方式打开文件
  if(fd < 0)  // 如果文件打开失败
    return -1;  // 返回-1表示错误
  r = fstat(fd, st);  // 获取文件描述符对应文件的状态信息
  close(fd);  // 关闭文件，释放文件描述符
  return r;  // 返回fstat的执行结果
}

// 字符串转整数（ASCII to Integer）函数
// 将字符串形式的数字转换为对应的整数值
int
atoi(const char *s)
{
  int n;  // 存储转换后的整数值（Integer value）

  n = 0;  // 初始化结果为0
  // 遍历字符串，直到遇到非数字字符
  while('0' <= *s && *s <= '9')
    // 将当前数字字符转换为整数值并累加到结果中
    // n*10: 将之前的结果左移一位（十进制）
    // *s - '0': 将字符'0'-'9'转换为数值0-9
    // s++: 移动到下一个字符
    n = n*10 + *s++ - '0';
  return n;  // 返回转换后的整数值
}

// 内存移动（Memory Move）函数
// 将源内存区域的数据移动到目标内存区域，处理内存重叠（Memory overlap）的情况
void*
memmove(void *vdst, const void *vsrc, int n)
{
  char *dst;  // 目标内存区域的字符指针（Destination pointer）
  const char *src;  // 源内存区域的字符指针（Source pointer）

  dst = vdst;  // 初始化目标指针
  src = vsrc;  // 初始化源指针
  // 如果源地址大于目标地址，说明没有内存重叠，从前向后复制
  if (src > dst) {
    while(n-- > 0)  // 从前向后复制每个字节
      *dst++ = *src++;  // 先赋值，然后两个指针都递增（Increment）
  } else {
    // 如果源地址小于等于目标地址，说明可能有内存重叠，从后向前复制
    dst += n;  // 将指针移动到目标区域的末尾
    src += n;  // 将指针移动到源区域的末尾
    while(n-- > 0)  // 从后向前复制每个字节
      *--dst = *--src;  // 先递减（Decrement）指针，然后赋值
  }
  return vdst;  // 返回目标内存区域的起始地址
}

// 内存比较（Memory Compare）函数
// 比较两个内存区域的前n个字节是否相等
int
memcmp(const void *s1, const void *s2, uint n)
{
  const char *p1 = s1, *p2 = s2;  // 将void指针转换为char指针
  // 循环比较n个字节
  while (n-- > 0) {
    // 如果当前字节不相等
    if (*p1 != *p2) {
      return *p1 - *p2;  // 返回两个字节的差值
    }
    p1++;  // 移动到下一个字节
    p2++;  // 移动到下一个字节
  }
  return 0;  // 所有字节都相等，返回0
}

// 内存复制函数
// 将源内存区域的数据复制到目标内存区域
// 注意：此函数使用memmove实现，可以处理内存重叠的情况
void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);  // 调用memmove函数完成内存复制
}

// 内存分配函数（立即分配）
// 分配n字节的内存空间，立即分配物理内存
char *
sbrk(int n) {
  return sys_sbrk(n, SBRK_EAGER);  // 调用系统调用，使用立即分配模式
}

// 内存分配函数（延迟分配）
// 分配n字节的内存空间，延迟分配物理内存（按需分配）
char *
sbrklazy(int n) {
  return sys_sbrk(n, SBRK_LAZY);  // 调用系统调用，使用延迟分配模式
}

