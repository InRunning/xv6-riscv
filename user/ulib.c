#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "kernel/vm.h"
#include "user/user.h"

//
// wrapper so that it's OK if main() does not call exit().
//
void
start(int argc, char **argv)
{
  int r;
  extern int main(int argc, char **argv);
  r = main(argc, argv);
  exit(r);
}

char*
strcpy(char *s, const char *t)
{
  char *os;

  os = s;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

int
strcmp(const char *p, const char *q)
{
  while(*p && *p == *q)
    p++, q++;
  return (uchar)*p - (uchar)*q;
}

// 计算字符串长度的函数
//
// 函数功能：
// 计算以空字符('\0')结尾的字符串的长度，不包括结尾的空字符。
// 这是C标准库中的一个基本函数，用于确定字符串中字符的数量。
//
// 参数说明：
// s - 指向以空字符结尾的字符串的常量字符指针
//    const修饰符表示函数不会修改传入的字符串内容
//
// 返回值：
// 返回一个无符号整数(uint)，表示字符串中的字符数量（不包括结尾的空字符）
//
// 实现原理：
// 1. 初始化计数器n为0
// 2. 使用for循环遍历字符串，直到遇到空字符('\0')为止
// 3. 每次循环检查s[n]是否为非零值（非空字符）
// 4. 如果是非空字符，计数器n递增
// 5. 当遇到空字符时，循环结束，返回计数器n的值
//
// 注意事项：
// - 调用者必须确保传入的字符串是以空字符结尾的，否则会导致未定义行为
// - 函数不会检查指针s是否为NULL，如果传入NULL指针会导致程序崩溃
// - 返回值类型是uint（无符号整数），可以处理很长的字符串
//
// 使用示例：
// char *str = "Hello";
// uint len = strlen(str);  // len的值为5
uint
strlen(const char *s)
{
  int n;  // 计数器，用于记录字符串长度

  // 遍历字符串，直到遇到空字符('\0')
  // s[n]会在遇到空字符时返回0，循环条件为假，循环结束
  for(n = 0; s[n]; n++)
    ;  // 空循环体，所有工作都在循环条件中完成
  return n;  // 返回字符串长度
}

void*
memset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;
  int i;
  for(i = 0; i < n; i++){
    cdst[i] = c;
  }
  return dst;
}

char*
strchr(const char *s, char c)
{
  for(; *s; s++)
    if(*s == c)
      return (char*)s;
  return 0;
}

char*
gets(char *buf, int max)
{
  int i, cc;
  char c;

  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);
    if(cc < 1)
      break;
    buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
stat(const char *n, struct stat *st)
{
  int fd;
  int r;

  fd = open(n, O_RDONLY);
  if(fd < 0)
    return -1;
  r = fstat(fd, st);
  close(fd);
  return r;
}

int
atoi(const char *s)
{
  int n;

  n = 0;
  while('0' <= *s && *s <= '9')
    n = n*10 + *s++ - '0';
  return n;
}

void*
memmove(void *vdst, const void *vsrc, int n)
{
  char *dst;
  const char *src;

  dst = vdst;
  src = vsrc;
  if (src > dst) {
    while(n-- > 0)
      *dst++ = *src++;
  } else {
    dst += n;
    src += n;
    while(n-- > 0)
      *--dst = *--src;
  }
  return vdst;
}

int
memcmp(const void *s1, const void *s2, uint n)
{
  const char *p1 = s1, *p2 = s2;
  while (n-- > 0) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

char *
sbrk(int n) {
  return sys_sbrk(n, SBRK_EAGER);
}

char *
sbrklazy(int n) {
  return sys_sbrk(n, SBRK_LAZY);
}

