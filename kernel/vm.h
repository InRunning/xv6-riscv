// 定义 sbrk 系统调用的内存分配策略
#define SBRK_EAGER 1 // 立即分配物理内存
#define SBRK_LAZY  2 // 惰性分配：仅增加进程大小，在发生页面错误时才分配物理内存
