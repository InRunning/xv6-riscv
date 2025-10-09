// Shell (Shell)
//
// 文件描述 (File Description)：
// 这是一个简单的 Unix shell (Unix Shell) 实现，支持基本的命令执行、重定向、管道和后台任务功能。
// 它是 xv6 操作系统中的用户级 shell 程序，提供了用户与操作系统交互的命令行界面。
//
// 主要功能 (Main Features)：
// - 命令执行 (Command Execution)：执行用户输入的各种命令
// - 输入/输出重定向 (Input/Output Redirection)：支持 <, >, >> 重定向操作符
// - 管道 (Pipes)：支持 | 操作符连接多个命令，实现进程间通信
// - 命令列表 (Command Lists)：支持 ; 操作符顺序执行多个命令
// - 后台执行 (Background Execution)：支持 & 操作符在后台执行命令
// - 内置命令 (Built-in Commands)：支持 cd 等内置命令
//
// 文件结构 (File Structure)：
// 1. 命令类型和结构体定义 (Command Types and Structure Definitions)
// 2. 命令执行函数 (Command Execution Functions)
// 3. 用户输入处理函数 (User Input Processing Functions)
// 4. 主函数 (Main Function)
// 5. 错误处理函数 (Error Handling Functions)
// 6. 命令构造函数 (Command Constructor Functions)
// 7. 命令解析函数 (Command Parsing Functions)
//
// 作者 (Author)：xv6 开发团队
// 许可证 (License)：遵循 xv6 操作系统的许可证

// 包含必要的头文件 (Include Necessary Headers)
#include "kernel/types.h"    // 包含基本的系统类型定义，如 int, char, uint 等
#include "user/user.h"      // 包含用户空间系统调用和库函数的声明
#include "kernel/fcntl.h"   // 包含文件控制选项的定义，如 O_RDONLY, O_WRONLY 等

// 命令类型常量定义 (Command Type Constants Definition)
// 解析后的命令表示 (Parsed Command Representation)
// 这些常量用于标识不同类型的命令结构，在命令解析和执行过程中起到关键作用

#define EXEC  1  // 执行普通命令 (Execute Command)
                 // 表示一个简单的命令执行，如 "ls", "echo hello" 等
                 // 对应的结构体类型：struct execcmd

#define REDIR 2  // 重定向命令 (Redirect Command)
                 // 表示包含输入/输出重定向的命令，如 "cat < input.txt", "ls > output.txt"
                 // 对应的结构体类型：struct redircmd

#define PIPE  3  // 管道命令 (Pipe Command)
                 // 表示通过管道连接的两个命令，如 "ls | grep pattern"
                 // 对应的结构体类型：struct pipecmd

#define LIST  4  // 命令列表（顺序执行）(Command List - Sequential Execution)
                 // 表示多个按顺序执行的命令，如 "ls; echo done"
                 // 对应的结构体类型：struct listcmd

#define BACK  5  // 后台执行命令 (Background Command)
                 // 表示在后台执行的命令，如 "sleep 10 &"
                 // 对应的结构体类型：struct backcmd

#define MAXARGS 10  // 命令参数的最大数量 (Maximum Number of Command Arguments)
                   // 限制一个命令可以拥有的参数数量，防止缓冲区溢出
                   // 包括命令名本身，所以实际最多可以有 9 个参数

// 基础命令结构体 (Base Command Structure)
// 所有命令类型都继承自这个结构 (All Command Types Inherit from This Structure)
// 这是一个通用的命令结构体，作为所有具体命令类型的基类
// 通过 type 字段可以区分不同的命令类型，实现多态性
struct cmd {
  int type;  // 命令类型 (Command Type)
             // 可以是 EXEC, REDIR, PIPE, LIST, BACK 中的一个
             // 用于在运行时确定命令的具体类型，以便进行相应的处理
};

// 执行命令结构体 (Execute Command Structure)
// 表示一个简单的命令执行，如 "ls", "echo hello" 等
struct execcmd {
  int type;              // 命令类型 (Command Type)，此处为 EXEC
                         // 标识这是一个执行命令结构体
  
  char *argv[MAXARGS];   // 命令参数数组 (Command Argument Array)
                         // 存储命令名及其参数，以 NULL 指针结尾
                         // 例如：对于 "echo hello world"，argv[0]="echo", argv[1]="hello", argv[2]="world", argv[3]=NULL
  
  char *eargv[MAXARGS];  // 参数结束位置指针数组 (Argument End Position Pointer Array)
                         // 存储每个参数字符串的结束位置，用于字符串终止
                         // 在 nulterminate 函数中，这些位置会被设置为空字符 '\0'
};

// 重定向命令结构体 (Redirect Command Structure)
// 表示包含输入/输出重定向的命令，如 "cat < input.txt", "ls > output.txt"
struct redircmd {
  int type;         // 命令类型 (Command Type)，此处为 REDIR
                   // 标识这是一个重定向命令结构体
  
  struct cmd *cmd;  // 要执行的子命令 (Subcommand to Execute)
                   // 指向实际要执行的命令，可以是任何类型的命令
                   // 例如：在 "ls > output.txt" 中，cmd 指向一个 execcmd 结构
  
  char *file;       // 重定向目标文件名 (Redirect Target Filename)
                   // 指向重定向目标的文件名字符串
                   // 例如：在 "ls > output.txt" 中，file 指向 "output.txt"
  
  char *efile;      // 文件名结束位置指针 (Filename End Position Pointer)
                   // 指向文件名字符串的结束位置，用于字符串终止
                   // 在 nulterminate 函数中，这个位置会被设置为空字符 '\0'
  
  int mode;         // 文件打开模式 (File Open Mode)
                   // 指定文件的打开方式，如 O_RDONLY, O_WRONLY, O_CREATE 等
                   // 例如：对于 ">" 操作符，mode 为 O_WRONLY|O_CREATE|O_TRUNC
  
  int fd;           // 要重定向的文件描述符 (File Descriptor to Redirect)
                   // 指定要重定向的标准文件描述符
                   // 0 表示标准输入，1 表示标准输出，2 表示标准错误
};

// 管道命令结构体 (Pipe Command Structure)
// 表示通过管道连接的两个命令，如 "ls | grep pattern"
struct pipecmd {
  int type;         // 命令类型 (Command Type)，此处为 PIPE
                   // 标识这是一个管道命令结构体
  
  struct cmd *left;  // 管道左侧命令（写入端）(Left Command of Pipe - Write End)
                    // 指向管道左侧的命令，其标准输出将被重定向到管道
                    // 例如：在 "ls | grep pattern" 中，left 指向一个 execcmd 结构（ls 命令）
  
  struct cmd *right; // 管道右侧命令（读取端）(Right Command of Pipe - Read End)
                    // 指向管道右侧的命令，其标准输入将从管道读取
                    // 例如：在 "ls | grep pattern" 中，right 指向一个 execcmd 结构（grep 命令）
};

// 命令列表结构体（顺序执行多个命令）(Command List Structure - Sequential Execution)
// 表示多个按顺序执行的命令，如 "ls; echo done"
struct listcmd {
  int type;         // 命令类型 (Command Type)，此处为 LIST
                   // 标识这是一个命令列表结构体
  
  struct cmd *left;  // 第一个要执行的命令 (First Command to Execute)
                   // 指向列表中的第一个命令
                   // 例如：在 "ls; echo done" 中，left 指向一个 execcmd 结构（ls 命令）
  
  struct cmd *right; // 第二个要执行的命令 (Second Command to Execute)
                    // 指向列表中的第二个命令
                    // 例如：在 "ls; echo done" 中，right 指向一个 execcmd 结构（echo 命令）
};

// 后台命令结构体 (Background Command Structure)
// 表示在后台执行的命令，如 "sleep 10 &"
struct backcmd {
  int type;         // 命令类型 (Command Type)，此处为 BACK
                   // 标识这是一个后台命令结构体
  
  struct cmd *cmd;  // 要在后台执行的命令 (Command to Execute in Background)
                   // 指向实际要在后台执行的命令，可以是任何类型的命令
                   // 例如：在 "sleep 10 &" 中，cmd 指向一个 execcmd 结构（sleep 命令）
};

// 函数声明 (Function Declarations)
// 以下是本文件中定义的主要函数的声明，按照功能分组

// 进程管理函数 (Process Management Functions)
int fork1(void);  // 创建子进程，失败时调用panic (Fork but panics on failure)
                 // 这是一个 fork() 系统调用的包装函数，在失败时调用 panic() 而不是返回错误
                 // 参数：无
                 // 返回值：在父进程中返回子进程的 PID，在子进程中返回 0

// 错误处理函数 (Error Handling Functions)
void panic(char*); // 错误处理函数，打印错误信息并退出 (Error Handling Function)
                  // 在遇到严重错误时调用，打印错误信息到标准错误并退出程序
                  // 参数：s - 指向错误信息字符串的指针
                  // 返回值：无（函数不会返回，因为会调用 exit()）

// 命令解析函数 (Command Parsing Functions)
struct cmd *parsecmd(char*); // 解析命令字符串，生成命令结构体 (Parse Command String)
                            // 这是命令解析的入口函数，将用户输入的命令字符串转换为内部命令结构
                            // 参数：s - 指向要解析的命令字符串的指针
                            // 返回值：指向解析后的命令结构的指针

// 命令执行函数 (Command Execution Functions)
void runcmd(struct cmd*) __attribute__((noreturn)); // 执行命令，永不返回 (Execute Command)
                                                    // 执行解析后的命令，根据命令类型进行相应的处理
                                                    // 参数：cmd - 指向要执行的命令结构的指针
                                                    // 返回值：无（函数不会返回，因为会调用 exec() 或 exit()）
                                                    // __attribute__((noreturn)) 告诉编译器这个函数不会返回

// Execute cmd.  Never returns.
// 执行解析后的命令，此函数永不返回（要么执行新程序，要么退出）
void
runcmd(struct cmd *cmd)
{
  int p[2];  // 管道文件描述符数组
  struct backcmd *bcmd;   // 后台命令指针
  struct execcmd *ecmd;   // 执行命令指针
  struct listcmd *lcmd;   // 命令列表指针
  struct pipecmd *pcmd;   // 管道命令指针
  struct redircmd *rcmd;  // 重定向命令指针

  // 检查命令是否为空
  if(cmd == 0)
    exit(1);

  // 根据命令类型执行相应的操作
  switch(cmd->type){
  default:
    panic("runcmd");  // 未知命令类型，报错退出

  case EXEC:  // 执行普通命令
    ecmd = (struct execcmd*)cmd;
    // 检查是否有命令要执行
    if(ecmd->argv[0] == 0)
      exit(1);
    // 尝试执行命令
    exec(ecmd->argv[0], ecmd->argv);
    // 如果exec返回，说明执行失败
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:  // 处理重定向命令
    rcmd = (struct redircmd*)cmd;
    // 关闭要重定向的文件描述符
    close(rcmd->fd);
    // 打开重定向目标文件
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    // 执行重定向后的命令
    runcmd(rcmd->cmd);
    break;

  case LIST:  // 处理命令列表（顺序执行）
    lcmd = (struct listcmd*)cmd;
    // 创建子进程执行第一个命令
    if(fork1() == 0)
      runcmd(lcmd->left);
    // 等待第一个命令完成
    wait(0);
    // 执行第二个命令
    runcmd(lcmd->right);
    break;

  case PIPE:  // 处理管道命令
    pcmd = (struct pipecmd*)cmd;
    // 创建管道
    if(pipe(p) < 0)
      panic("pipe");
    // 创建子进程执行管道左侧命令（写入端）
    if(fork1() == 0){
      close(1);        // 关闭标准输出
      dup(p[1]);       // 将管道写入端复制到标准输出
      close(p[0]);     // 关闭管道读取端
      close(p[1]);     // 关闭管道写入端
      runcmd(pcmd->left);  // 执行左侧命令
    }
    // 创建子进程执行管道右侧命令（读取端）
    if(fork1() == 0){
      close(0);        // 关闭标准输入
      dup(p[0]);       // 将管道读取端复制到标准输入
      close(p[0]);     // 关闭管道读取端
      close(p[1]);     // 关闭管道写入端
      runcmd(pcmd->right); // 执行右侧命令
    }
    // 父进程关闭管道两端
    close(p[0]);
    close(p[1]);
    // 等待两个子进程结束
    wait(0);
    wait(0);
    break;

  case BACK:  // 处理后台命令
    bcmd = (struct backcmd*)cmd;
    // 创建子进程在后台执行命令
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

// 从用户输入获取命令
//
// 函数功能：
// 这个函数负责从标准输入读取用户输入的命令，并将其存储在提供的缓冲区中。
// 它首先显示shell提示符，然后等待用户输入，最后处理输入并返回状态。
//
// 参数说明：
// buf - 字符指针，指向用于存储用户输入命令的缓冲区
//       调用者需要确保这个缓冲区有足够的空间来存储用户输入
//       函数执行完成后，缓冲区将包含用户输入的命令字符串（以换行符结尾）
//
// nbuf - 整数，表示缓冲区的大小（以字节为单位）
//       这个参数用于防止缓冲区溢出，确保不会读取超过缓冲区容量的数据
//       gets函数会使用这个值来限制读取的字符数量
//
// 返回值：
// 0 - 成功获取到用户输入的命令
// -1 - 遇到EOF（文件结束符），通常是用户输入了Ctrl+D
//
// 工作流程：
// 1. 向标准错误（文件描述符2）写入shell提示符"$ "
// 2. 使用memset将缓冲区清零，确保没有残留数据
// 3. 调用gets从标准输入读取用户输入，最多读取nbuf-1个字符
// 4. 检查缓冲区的第一个字符是否为0（空字符），如果是则表示EOF
// 5. 根据检查结果返回相应的状态码
//
// 注意事项：
// - 提示符写入标准错误而不是标准输出，这样即使输出被重定向，用户仍能看到提示符
// - 用户输入的命令会包含末尾的换行符（\n），调用者需要注意处理
// - 如果用户输入超过缓冲区大小，gets会自动截断，防止缓冲区溢出
int
getcmd(char *buf, int nbuf)
{
  write(2, "$ ", 2);  // 显示shell提示符
  // 参数详解：
  // 第一个参数2：文件描述符，指定写入目标
  //   在Unix系统中，每个进程默认有三个标准文件描述符：
  //   0 - 标准输入(stdin)
  //   1 - 标准输出(stdout)
  //   2 - 标准错误(stderr)
  //   这里使用2表示将提示符写入标准错误流，而不是标准输出
  //
  // 第二个参数"$ "：要写入的字符串，即shell提示符
  //
  // 第三个参数2：要写入的字节数
  //   这里指定写入2个字节：'$'字符和空格' '字符
  //   注意：这个数字不包括字符串末尾的空字符'\0'
  //
  // 为什么写入标准错误而不是标准输出？
  // 这样做的好处是即使用户将标准输出重定向到文件（如：ls > file.txt），
  // shell提示符仍然会显示在终端上，不会被重定向到文件中。
  memset(buf, 0, nbuf);  // 清空缓冲区，将所有字节设置为0（空字符）
  gets(buf, nbuf);  // 从标准输入读取用户输入，最多读取nbuf-1个字符，自动添加字符串结束符
  if(buf[0] == 0) // 检查缓冲区第一个字符是否为空字符，如果是则表示EOF（用户输入Ctrl+D）
    return -1;     // 返回-1表示EOF，shell应该退出
  return 0;        // 返回0表示成功获取到命令
}

// Shell主函数
int
main(void)
{
  static char buf[100];  // 命令缓冲区
  int fd;

  // 确保三个标准文件描述符（0,1,2）是打开的
  // 在Unix系统中，每个进程默认应该有三个标准文件描述符：
  // 0 - 标准输入(stdin)
  // 1 - 标准输出(stdout)
  // 2 - 标准错误(stderr)
  //
  // 当shell启动时，这些文件描述符可能没有被正确设置，或者可能被关闭了。
  // 这段代码的作用是确保这三个标准文件描述符都被正确打开并指向console设备。
  //
  // 工作原理：
  // 1. 循环打开console设备文件，每次open()会返回最小的可用文件描述符
  // 2. 如果标准文件描述符0、1、2中有任何一个未被打开，open()会返回该描述符
  // 3. 如果所有标准文件描述符都已打开，open()会返回3或更大的值
  //
  // 例如：
  // - 如果fd=0，说明标准输入未打开，现在已经被打开
  // - 如果fd=1，说明标准输出未打开，现在已经被打开
  // - 如果fd=2，说明标准错误未打开，现在已经被打开
  // - 如果fd>=3，说明所有标准文件描述符都已打开，这个新打开的描述符是多余的，需要关闭
  //
  // 这样可以确保shell启动时，无论初始状态如何，最终都会有正确的标准输入、输出和错误流。
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){  // 如果文件描述符大于等于3，说明标准描述符(0,1,2)已经全部打开
      close(fd);  // 关闭这个多余的描述符，避免资源浪费
      break;      // 所有标准描述符已就绪，退出循环
    }
    // 如果fd<3，说明某个标准描述符刚刚被打开，继续循环检查其他描述符
  }

  // 循环读取并执行用户输入的命令
  while(getcmd(buf, sizeof(buf)) >= 0){
    char *cmd = buf;
    // 跳过命令开头的空白字符
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    // 如果是空命令（只有换行符），则跳过
    if (*cmd == '\n') // is a blank command
      continue;
    // 处理cd命令（必须在父进程中执行，不能在子进程中）
    if(cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' '){
      // Chdir must be called by the parent, not the child.
      cmd[strlen(cmd)-1] = 0;  // 去掉末尾的换行符
      // 尝试切换目录
      if(chdir(cmd+3) < 0)
        fprintf(2, "cannot cd %s\n", cmd+3);
    } else {
      // 对于其他命令，创建子进程执行
      if(fork1() == 0)
        runcmd(parsecmd(cmd));  // 解析并执行命令
      wait(0);  // 等待子进程结束
    }
  }
  exit(0);
}

// 错误处理函数
// 打印错误信息到标准错误并退出
// s: 错误信息字符串
void
panic(char *s)
{
  fprintf(2, "%s\n", s);  // 将错误信息输出到标准错误（文件描述符2）
  exit(1);  // 以错误状态退出
}

// 创建子进程的包装函数
// 如果fork失败，调用panic函数报错退出
// 返回值: 在父进程中返回子进程PID，在子进程中返回0
int
fork1(void)
{
  int pid;

  pid = fork();  // 创建子进程
  if(pid == -1)  // fork失败
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

// 命令解析函数 (Command Parse Function)
//
// 函数功能 (Function Description)：
// 这是shell (Shell) 命令解析的入口函数，负责将用户输入的命令字符串转换为
// 内部命令结构体（struct cmd - command structure），以便后续执行。
//
// 参数说明 (Parameter Description)：
// s - 指向用户输入的命令字符串的指针 (pointer to the command string)
//    这个字符串应该包含一个完整的shell命令，可能包含重定向 (redirection)、
//    管道 (pipe) 等操作
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，表示解析后的命令结构
// 这个结构体可以是execcmd (execute command)、redircmd (redirect command)、
// pipecmd (pipe command)、listcmd (list command) 或 backcmd (background command) 中的一种
// 具体类型取决于命令的复杂程度
//
// 工作流程 (Workflow)：
// 1. 计算字符串的结束位置 (calculate the end position of the string)
// 2. 调用parseline (parse line) 函数解析命令行
// 3. 检查是否所有输入都被解析（没有剩余字符）
// 4. 如果有未解析的字符，报告语法错误 (syntax error)
// 5. 对解析后的命令结构进行空字符终止处理 (null termination)
// 6. 返回解析后的命令结构
//
// 错误处理 (Error Handling)：
// 如果命令字符串包含语法错误，函数会调用panic (panic) 函数终止程序
// 这通常发生在命令格式不正确或包含无法识别的字符时
//
// 注意事项 (Notes)：
// - 函数会修改传入的字符串指针s（通过指针的指针）
// - 调用者不需要释放返回的命令结构，它会在命令执行后被自动释放
// - 函数依赖于一系列辅助解析函数（parseline、peek、nulterminate等）
struct cmd*
parsecmd(char *s)
{
  char *es;        // 字符串结束指针 (end of string)，指向输入字符串的末尾
  struct cmd *cmd; // 解析后的命令结构体指针 (command structure pointer)

  // 计算字符串的结束位置
  // strlen(s) (string length) 返回字符串长度，s + strlen(s)指向字符串末尾的空字符
  es = s + strlen(s);
  
  // 调用parseline (parse line) 函数解析命令行
  // &s传递字符串指针的地址，这样parseline可以修改s的值（跳过已解析的部分）
  // es作为字符串结束位置，防止解析越界
  // parseline会递归地解析命令，处理管道、列表、后台执行等复杂结构
  cmd = parseline(&s, es);
  
  // 检查是否还有未解析的字符
  // peek (peek) 函数检查当前位置是否还有非空白字符
  // 空字符串""作为参数表示检查任何非空白字符
  peek(&s, es, "");
  
  // 如果s不等于es，说明有未解析的字符剩余
  // 这通常表示命令语法错误 (syntax error)，例如不完整的命令或无法识别的字符
  if(s != es){
    // 将剩余的未解析字符输出到标准错误 (standard error)，帮助用户调试
    fprintf(2, "leftovers: %s\n", s);
    // 调用panic (panic) 函数终止程序，报告语法错误
    panic("syntax");
  }
  
  // 对解析后的命令结构进行空字符终止处理 (null termination)
  // nulterminate (null terminate) 函数会遍历命令结构中的所有字符串，
  // 确保它们正确地以空字符结尾
  // 这是为了后续执行命令时能够正确处理字符串
  nulterminate(cmd);
  
  // 返回解析后的命令结构
  return cmd;
}

// 解析命令行函数 (Parse Line Function)
//
// 函数功能 (Function Description)：
// 这个函数负责解析一个完整的命令行，处理后台执行（&）和命令列表（;）操作符。
// 它是命令解析过程中的重要一环，位于解析管道和解析执行命令之间。
//
// 参数说明 (Parameter Description)：
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     这是一个双重指针，允许函数修改原始字符串指针的位置
//     函数会更新这个指针，使其指向已解析部分的下一个字符
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止解析越界
//     函数不会修改这个指针
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，表示解析后的命令结构
// 根据命令内容，可能是以下类型之一：
// - execcmd (execute command) - 简单执行命令
// - pipecmd (pipe command) - 管道命令
// - listcmd (list command) - 命令列表（由分号分隔）
// - backcmd (background command) - 后台执行命令
//
// 工作流程 (Workflow)：
// 1. 首先调用parsepipe函数解析可能的管道命令
// 2. 检查是否有后台执行操作符（&），如果有则创建后台命令
// 3. 检查是否有命令列表操作符（;），如果有则递归解析后续命令并创建命令列表
// 4. 返回最终构建的命令结构
//
// 解析优先级 (Parsing Priority)：
// 函数按照以下优先级处理操作符：
// 1. 管道（|）- 在parsepipe中处理，优先级最高
// 2. 后台执行（&）- 优先级次之
// 3. 命令列表（;）- 优先级最低
//
// 示例 (Examples)：
// - "ls" -> 返回execcmd结构
// - "ls &" -> 返回backcmd结构，包含execcmd
// - "ls; pwd" -> 返回listcmd结构，包含两个execcmd
// - "ls | wc; pwd" -> 返回listcmd结构，包含pipecmd和execcmd
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其跳过已解析的部分
// - 函数使用递归调用来处理命令列表
// - 函数依赖于parsepipe、peek、gettoken、backcmd和listcmd等辅助函数
struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;  // 解析后的命令结构体指针

  // 首先调用parsepipe (parse pipe) 函数解析可能的管道命令
  // 管道操作符（|）的优先级高于后台执行和命令列表
  // 例如：在"ls | grep x &"中，先解析"ls | grep x"，再处理后台执行
  cmd = parsepipe(ps, es);
  
  // 检查是否有后台执行操作符（&）
  // peek (peek) 函数检查当前位置是否是"&"字符
  // 使用while循环可以处理多个连续的&操作符（虽然这在语法上不太常见）
  while(peek(ps, es, "&")){
    // 使用gettoken (get token) 函数消耗掉"&"字符
    // 参数0,0表示我们不关心token的具体内容，只是跳过它
    gettoken(ps, es, 0, 0);
    // 将当前命令包装为后台命令
    // backcmd (background command) 创建一个backcmd结构，将原命令作为子命令
    cmd = backcmd(cmd);
  }
  
  // 检查是否有命令列表操作符（;）
  // 分号用于分隔多个顺序执行的命令
  // 例如："ls; pwd"表示先执行ls，再执行pwd
  if(peek(ps, es, ";")){
    // 使用gettoken函数消耗掉";"字符
    gettoken(ps, es, 0, 0);
    // 递归调用parseline解析分号后的命令
    // listcmd (list command) 创建一个listcmd结构，将当前命令和后续命令作为左右子命令
    cmd = listcmd(cmd, parseline(ps, es));
  }
  
  // 返回最终构建的命令结构
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
