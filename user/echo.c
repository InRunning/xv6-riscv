#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  // echo 用户程序会把每个命令行参数写到标准输出(文件描述符 1)。
  // 缩写说明：argc(Argument Count 参数个数)、argv(Argument Vector 参数向量字符串数组)。
  int i; // i(Index 索引) 用于遍历参数

  // 从 1 开始跳过 argv[0]（程序名），依次打印参数。
  for(i = 1; i < argc; i++){
    // write(fd, buf, len)：将缓冲区内容写到文件描述符。
    write(1, argv[i], strlen(argv[i])); // fd=1 → stdout 标准输出
    if(i + 1 < argc){
      // 参数之间输出空格
      write(1, " ", 1);
    } else {
      // 最后一个参数后输出换行
      write(1, "\n", 1);
    }
  }
  exit(0); // 正常退出
}
