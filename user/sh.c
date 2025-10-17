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
#include "kernel/types.h" // 包含基本的系统类型定义，如 int, char, uint 等
#include "user/user.h"    // 包含用户空间系统调用和库函数的声明
#include "kernel/fcntl.h" // 包含文件控制选项的定义，如 O_RDONLY, O_WRONLY 等

// 命令类型常量定义 (Command Type Constants Definition)
// 解析后的命令表示 (Parsed Command Representation)
// 这些常量用于标识不同类型的命令结构，在命令解析和执行过程中起到关键作用

#define EXEC 1 // 执行普通命令 (Execute Command)
               // 表示一个简单的命令执行，如 "ls", "echo hello" 等
               // 对应的结构体类型：struct execcmd

#define REDIR 2 // 重定向命令 (Redirect Command)
                // 表示包含输入/输出重定向的命令，如 "cat < input.txt", "ls > output.txt"
                // 对应的结构体类型：struct redircmd

#define PIPE 3 // 管道命令 (Pipe Command)
               // 表示通过管道连接的两个命令，如 "ls | grep pattern"
               // 对应的结构体类型：struct pipecmd

#define LIST 4 // 命令列表（顺序执行）(Command List - Sequential Execution)
               // 表示多个按顺序执行的命令，如 "ls; echo done"
               // 对应的结构体类型：struct listcmd

#define BACK 5 // 后台执行命令 (Background Command)
               // 表示在后台执行的命令，如 "sleep 10 &"
               // 对应的结构体类型：struct backcmd

#define MAXARGS 10 // 命令参数的最大数量 (Maximum Number of Command Arguments)
                   // 限制一个命令可以拥有的参数数量，防止缓冲区溢出
                   // 包括命令名本身，所以实际最多可以有 9 个参数

// 基础命令结构体 (Base Command Structure)
// 所有命令类型都继承自这个结构 (All Command Types Inherit from This Structure)
// 这是一个通用的命令结构体，作为所有具体命令类型的基类
// 通过 type 字段可以区分不同的命令类型，实现多态性
struct cmd
{
  int type; // 命令类型 (Command Type)
            // 可以是 EXEC, REDIR, PIPE, LIST, BACK 中的一个
            // 用于在运行时确定命令的具体类型，以便进行相应的处理
};

// 执行命令结构体 (Execute Command Structure)
// 表示一个简单的命令执行，如 "ls", "echo hello" 等
struct execcmd
{
  int type; // 命令类型 (Command Type)，此处为 EXEC
            // 标识这是一个执行命令结构体

  char *argv[MAXARGS]; // 命令参数数组 (Command Argument Array)
                       // 存储命令名及其参数，以 NULL 指针结尾
                       // 例如：对于 "echo hello world"，argv[0]="echo", argv[1]="hello", argv[2]="world", argv[3]=NULL

  char *eargv[MAXARGS]; // 参数结束位置指针数组 (Argument End Position Pointer Array)
                        // 存储每个参数字符串的结束位置，用于字符串终止
                        // 在 nulterminate 函数中，这些位置会被设置为空字符 '\0'
};

// 重定向命令结构体 (Redirect Command Structure)
// 表示包含输入/输出重定向的命令，如 "cat < input.txt", "ls > output.txt"
struct redircmd
{
  int type; // 命令类型 (Command Type)，此处为 REDIR
            // 标识这是一个重定向命令结构体

  struct cmd *cmd; // 要执行的子命令 (Subcommand to Execute)
                   // 指向实际要执行的命令，可以是任何类型的命令
                   // 例如：在 "ls > output.txt" 中，cmd 指向一个 execcmd 结构

  char *file; // 重定向目标文件名 (Redirect Target Filename)
              // 指向重定向目标的文件名字符串
              // 例如：在 "ls > output.txt" 中，file 指向 "output.txt"

  char *efile; // 文件名结束位置指针 (Filename End Position Pointer)
               // 指向文件名字符串的结束位置，用于字符串终止
               // 在 nulterminate 函数中，这个位置会被设置为空字符 '\0'

  int mode; // 文件打开模式 (File Open Mode)
            // 指定文件的打开方式，如 O_RDONLY, O_WRONLY, O_CREATE 等
            // 例如：对于 ">" 操作符，mode 为 O_WRONLY|O_CREATE|O_TRUNC

  int fd; // 要重定向的文件描述符 (File Descriptor to Redirect)
          // 指定要重定向的标准文件描述符
          // 0 表示标准输入，1 表示标准输出，2 表示标准错误
};

// 管道命令结构体 (Pipe Command Structure)
// 表示通过管道连接的两个命令，如 "ls | grep pattern"
struct pipecmd
{
  int type; // 命令类型 (Command Type)，此处为 PIPE
            // 标识这是一个管道命令结构体

  struct cmd *left; // 管道左侧命令（写入端）(Left Command of Pipe - Write End)
                    // 指向管道左侧的命令，其标准输出将被重定向到管道
                    // 例如：在 "ls | grep pattern" 中，left 指向一个 execcmd 结构（ls 命令）

  struct cmd *right; // 管道右侧命令（读取端）(Right Command of Pipe - Read End)
                     // 指向管道右侧的命令，其标准输入将从管道读取
                     // 例如：在 "ls | grep pattern" 中，right 指向一个 execcmd 结构（grep 命令）
};

// 命令列表结构体（顺序执行多个命令）(Command List Structure - Sequential Execution)
// 表示多个按顺序执行的命令，如 "ls; echo done"
struct listcmd
{
  int type; // 命令类型 (Command Type)，此处为 LIST
            // 标识这是一个命令列表结构体

  struct cmd *left; // 第一个要执行的命令 (First Command to Execute)
                    // 指向列表中的第一个命令
                    // 例如：在 "ls; echo done" 中，left 指向一个 execcmd 结构（ls 命令）

  struct cmd *right; // 第二个要执行的命令 (Second Command to Execute)
                     // 指向列表中的第二个命令
                     // 例如：在 "ls; echo done" 中，right 指向一个 execcmd 结构（echo 命令）
};

// 后台命令结构体 (Background Command Structure)
// 表示在后台执行的命令，如 "sleep 10 &"
struct backcmd
{
  int type; // 命令类型 (Command Type)，此处为 BACK
            // 标识这是一个后台命令结构体

  struct cmd *cmd; // 要在后台执行的命令 (Command to Execute in Background)
                   // 指向实际要在后台执行的命令，可以是任何类型的命令
                   // 例如：在 "sleep 10 &" 中，cmd 指向一个 execcmd 结构（sleep 命令）
};

// 函数声明 (Function Declarations)
// 以下是本文件中定义的主要函数的声明，按照功能分组

// 进程管理函数 (Process Management Functions)
int fork1(void); // 创建子进程，失败时调用panic (Fork but panics on failure)
                 // 这是一个 fork() 系统调用的包装函数，在失败时调用 panic() 而不是返回错误
                 // 参数：无
                 // 返回值：在父进程中返回子进程的 PID，在子进程中返回 0

// 错误处理函数 (Error Handling Functions)
void panic(char *); // 错误处理函数，打印错误信息并退出 (Error Handling Function)
                    // 在遇到严重错误时调用，打印错误信息到标准错误并退出程序
                    // 参数：s - 指向错误信息字符串的指针
                    // 返回值：无（函数不会返回，因为会调用 exit()）

// 命令解析函数 (Command Parsing Functions)
struct cmd *parsecmd(char *); // 解析命令字符串，生成命令结构体 (Parse Command String)
                              // 这是命令解析的入口函数，将用户输入的命令字符串转换为内部命令结构
                              // 参数：s - 指向要解析的命令字符串的指针
                              // 返回值：指向解析后的命令结构的指针

// 命令执行函数 (Command Execution Functions)
void runcmd(struct cmd *) __attribute__((noreturn)); // 执行命令，永不返回 (Execute Command)
                                                     // 执行解析后的命令，根据命令类型进行相应的处理
                                                     // 参数：cmd - 指向要执行的命令结构的指针
                                                     // 返回值：无（函数不会返回，因为会调用 exec() 或 exit()）
                                                     // __attribute__((noreturn)) 告诉编译器这个函数不会返回

// 命令执行函数 (Command Execution Function)
//
// 函数功能 (Function Description)：
// 这是shell的核心执行函数，负责执行解析后的命令结构。
// 函数根据命令的类型（EXEC、REDIR、PIPE、LIST、BACK）进行相应的处理。
// 由于此函数会调用exec()或exit()，它永远不会返回到调用者。
//
// 参数说明 (Parameter Description)：
// cmd - 指向要执行的命令结构的指针
//      这个指针可以指向任何类型的命令结构（execcmd、redircmd、pipecmd、listcmd、backcmd）
//      通过cmd->type字段可以确定具体的命令类型
//
// 返回值 (Return Value)：
// 无返回值，因为函数会调用exec()执行新程序或调用exit()退出当前进程
// __attribute__((noreturn)) 告诉编译器这个函数不会返回
//
// 工作流程 (Workflow)：
// 1. 检查命令指针是否为空，如果为空则退出
// 2. 根据命令类型（cmd->type）进行相应的处理：
//    - EXEC：执行普通命令，调用exec()系统调用
//    - REDIR：处理重定向命令，先设置重定向再执行子命令
//    - LIST：处理命令列表，顺序执行多个命令
//    - PIPE：处理管道命令，创建两个子进程并通过管道连接
//    - BACK：处理后台命令，创建子进程在后台执行
// 3. 如果命令类型未知，调用panic()报错退出
//
// 错误处理 (Error Handling)：
// - 如果命令指针为空，直接退出
// - 如果exec()执行失败，打印错误信息并退出
// - 如果打开重定向文件失败，打印错误信息并退出
// - 如果创建管道失败，调用panic()报错退出
// - 如果遇到未知命令类型，调用panic()报错退出
//
// 注意事项 (Notes)：
// - 函数会修改当前进程的文件描述符（特别是在处理重定向和管道时）
// - 函数会创建子进程来执行命令（特别是在处理LIST、PIPE和BACK命令时）
// - 函数不会返回，要么执行新程序，要么退出当前进程
void runcmd(struct cmd *cmd)
{
  int p[2]; // 管道文件描述符数组 (Pipe File Descriptor Array)
            // 用于存储管道的读取端和写入端文件描述符
            // p[0] 是读取端，p[1] 是写入端

  // 命令结构体指针 (Command Structure Pointers)
  // 这些指针用于在switch语句中根据命令类型进行类型转换
  struct backcmd *bcmd;  // 后台命令指针 (Background Command Pointer)
  struct execcmd *ecmd;  // 执行命令指针 (Execute Command Pointer)
  struct listcmd *lcmd;  // 命令列表指针 (List Command Pointer)
  struct pipecmd *pcmd;  // 管道命令指针 (Pipe Command Pointer)
  struct redircmd *rcmd; // 重定向命令指针 (Redirect Command Pointer)

  // 检查命令是否为空 (Check if Command is Null)
  // 如果命令指针为空，说明没有有效的命令要执行，直接退出
  if (cmd == 0)
    exit(1);

  // 根据命令类型执行相应的操作 (Execute Based on Command Type)
  // 使用switch语句根据命令类型进行分发处理
  switch (cmd->type)
  {
  default:
    panic("runcmd"); // 未知命令类型，报错退出 (Unknown Command Type)

  case EXEC: // 执行普通命令 (Execute Simple Command)
    // 类型转换，将通用命令指针转换为执行命令指针
    ecmd = (struct execcmd *)cmd;

    // 检查是否有命令要执行 (Check if There is a Command to Execute)
    // 如果argv[0]为空，说明没有指定要执行的程序，直接退出
    if (ecmd->argv[0] == 0)
      exit(1);

    // 尝试执行命令 (Try to Execute the Command)
    // exec()系统调用会用新程序替换当前进程的映像
    // 如果exec()成功，当前进程会被新程序替换，不会返回
    // 如果exec()失败，会返回-1，并继续执行下面的代码
    exec(ecmd->argv[0], ecmd->argv);

    // 如果exec返回，说明执行失败 (If exec Returns, Execution Failed)
    // 打印错误信息到标准错误，帮助用户调试
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR: // 处理重定向命令 (Handle Redirect Command)
    // 类型转换，将通用命令指针转换为重定向命令指针
    rcmd = (struct redircmd *)cmd;

    // 关闭要重定向的文件描述符 (Close File Descriptor to Redirect)
    // 例如，如果要重定向标准输出（fd=1），先关闭当前的标准输出
    close(rcmd->fd);

    // 打开重定向目标文件 (Open Redirect Target File)
    // open() 会挑选“当前未被占用的最小文件描述符”。
    // 因为刚刚 close(rcmd->fd) 把目标编号空出来，所以 open() 几乎总会
    // 把新文件放在同一个编号上（套用了 Unix 的“最小可用 fd” 规则），
    // 从而使后续对标准输入/输出/错误的读写自动转向新文件。
    if (open(rcmd->file, rcmd->mode) < 0)
    {
      // 如果打开文件失败，打印错误信息并退出
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }

    // 执行重定向后的命令 (Execute the Redirected Command)
    // 递归调用runcmd执行子命令，此时重定向已经设置好
    runcmd(rcmd->cmd);
    break;

  case LIST: // 处理命令列表（顺序执行）(Handle Command List - Sequential Execution)
    // 类型转换，将通用命令指针转换为命令列表指针
    lcmd = (struct listcmd *)cmd;

    // 创建子进程执行第一个命令 (Create Child Process to Execute First Command)
    // 使用fork1()创建子进程，在子进程中执行左侧命令
    if (fork1() == 0)
      runcmd(lcmd->left);

    // 等待第一个命令完成 (Wait for First Command to Complete)
    // 父进程调用wait()等待子进程结束，确保命令按顺序执行
    wait(0);

    // 执行第二个命令 (Execute Second Command)
    // 在当前进程中执行右侧命令，不需要创建新的子进程
    runcmd(lcmd->right);
    break;

  case PIPE: // 处理管道命令 (Handle Pipe Command)
    // 类型转换，将通用命令指针转换为管道命令指针
    pcmd = (struct pipecmd *)cmd;

    // 创建管道 (Create Pipe)
    // pipe()系统调用创建一个管道，返回两个文件描述符
    // p[0]是读取端，p[1]是写入端
    if (pipe(p) < 0)
      panic("pipe");

    // 创建子进程执行管道左侧命令（写入端）(Create Child Process for Left Command - Write End)
    if (fork1() == 0)
    {
      close(1);           // 关闭标准输出 (Close Standard Output)
      dup(p[1]);          // 将管道写入端复制到标准输出 (Duplicate Pipe Write End to Standard Output)
      close(p[0]);        // 关闭管道读取端 (Close Pipe Read End)
      close(p[1]);        // 关闭管道写入端 (Close Pipe Write End)
      runcmd(pcmd->left); // 执行左侧命令 (Execute Left Command)
    }

    // 创建子进程执行管道右侧命令（读取端）(Create Child Process for Right Command - Read End)
    if (fork1() == 0)
    {
      close(0);            // 关闭标准输入 (Close Standard Input)
      dup(p[0]);           // 将管道读取端复制到标准输入 (Duplicate Pipe Read End to Standard Input)
      close(p[0]);         // 关闭管道读取端 (Close Pipe Read End)
      close(p[1]);         // 关闭管道写入端 (Close Pipe Write End)
      runcmd(pcmd->right); // 执行右侧命令 (Execute Right Command)
    }

    // 父进程关闭管道两端 (Parent Process Closes Both Ends of Pipe)
    // 父进程不需要使用管道，关闭两端以避免资源泄漏
    close(p[0]);
    close(p[1]);

    // 等待两个子进程结束 (Wait for Both Child Processes to Finish)
    // 父进程需要等待两个子进程都结束，确保管道命令执行完成
    wait(0);
    wait(0);
    break;

  case BACK: // 处理后台命令 (Handle Background Command)
    // 类型转换，将通用命令指针转换为后台命令指针
    bcmd = (struct backcmd *)cmd;

    // 创建子进程在后台执行命令 (Create Child Process to Execute Command in Background)
    // 使用fork1()创建子进程，在子进程中执行命令
    // 父进程不等待子进程结束，直接继续执行
    if (fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }

  // 如果执行到这里，说明命令执行完成但没有调用exec()
  // 这种情况通常发生在命令执行失败或者不需要执行新程序的情况下
  // 调用exit(0)正常退出当前进程
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
int getcmd(char *buf, int nbuf)
{
  write(2, "$ ", 2); // 显示shell提示符
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
  memset(buf, 0, nbuf); // 清空缓冲区，将所有字节设置为0（空字符）
  gets(buf, nbuf);      // 从标准输入读取用户输入，最多读取nbuf-1个字符，自动添加字符串结束符
  if (buf[0] == 0)      // 检查缓冲区第一个字符是否为空字符，如果是则表示EOF（用户输入Ctrl+D）
    return -1;          // 返回-1表示EOF，shell应该退出
  return 0;             // 返回0表示成功获取到命令
}

// Shell主函数 (Shell Main Function)
//
// 函数功能 (Function Description)：
// 这是shell程序的入口点，负责初始化shell环境并进入主循环，
// 不断读取用户输入的命令并执行。
//
// 参数说明 (Parameter Description)：
// 无参数 (No Parameters)
//
// 返回值 (Return Value)：
// 虽然函数声明为返回int，但实际上函数会调用exit(0)退出，
// 不会返回到调用者。
//
// 工作流程 (Workflow)：
// 1. 初始化标准文件描述符（0,1,2），确保它们都指向console设备
// 2. 进入主循环，不断读取用户输入的命令
// 3. 对每个命令进行预处理（跳过空白字符，检查空命令）
// 4. 特殊处理cd命令（必须在父进程中执行）
// 5. 对于其他命令，创建子进程执行
// 6. 等待子进程结束，继续下一轮循环
//
// 错误处理 (Error Handling)：
// - 如果cd命令执行失败，打印错误信息到标准错误
// - 如果命令解析或执行失败，由子进程处理错误，父进程继续运行
//
// 注意事项 (Notes)：
// - cd命令必须在父进程中执行，因为它需要改变当前进程的工作目录
// - 其他命令在子进程中执行，避免影响shell本身的状态
// - 使用静态缓冲区存储命令，避免每次循环都重新分配内存
int main(void)
{
  static char buf[100]; // 命令缓冲区 (Command Buffer)
                        // 使用静态存储，避免每次循环都重新分配内存
                        // 缓冲区大小为100字节，足够存储大多数shell命令
  int fd;               // 文件描述符变量 (File Descriptor Variable)
                        // 用于在初始化过程中打开console设备文件

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
  while ((fd = open("console", O_RDWR)) >= 0)
  {
    if (fd >= 3)
    {            // 如果文件描述符大于等于3，说明标准描述符(0,1,2)已经全部打开
      close(fd); // 关闭这个多余的描述符，避免资源浪费
      break;     // 所有标准描述符已就绪，退出循环
    }
    // 如果fd<3，说明某个标准描述符刚刚被打开，继续循环检查其他描述符
  }

  // 循环读取并执行用户输入的命令 (Main Command Loop)
  // 这是一个无限循环，直到getcmd返回-1（用户输入Ctrl+D）才会退出
  while (getcmd(buf, sizeof(buf)) >= 0)
  {
    char *cmd = buf; // 命令指针，指向缓冲区中的命令字符串
                     // 使用局部变量cmd而不是直接使用buf，便于移动指针

    // 跳过命令开头的空白字符 (Skip Leading Whitespace)
    // 去除命令开头的空格和制表符，使命令解析更加健壮
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;

    // 如果是空命令（只有换行符），则跳过 (Skip Empty Commands)
    // 如果命令只包含换行符，说明用户只是按了回车键，没有输入实际命令
    if (*cmd == '\n') // is a blank command
      continue;

    // 处理cd命令（必须在父进程中执行，不能在子进程中）
    // cd命令是一个特殊的内置命令，它需要改变当前进程的工作目录
    // 如果在子进程中执行，只会改变子进程的工作目录，而不会影响父进程（shell）
    if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' ')
    {
      // Chdir must be called by the parent, not the child.
      cmd[strlen(cmd) - 1] = 0; // 去掉末尾的换行符，将字符串截断
                                // strlen(cmd)-1指向换行符，将其设置为0（空字符）

      // 尝试切换目录 (Try to Change Directory)
      // chdir系统调用改变当前进程的工作目录
      // cmd+3指向目录名（跳过"cd "）
      if (chdir(cmd + 3) < 0)
        fprintf(2, "cannot cd %s\n", cmd + 3); // 如果失败，打印错误信息到标准错误
    }
    else
    {
      // 对于其他命令，创建子进程执行 (Execute Other Commands in Child Process)
      // 使用fork1创建子进程，在子进程中执行命令
      // 父进程等待子进程结束，确保命令按顺序执行
      if (fork1() == 0)
        runcmd(parsecmd(cmd)); // 解析并执行命令
                               // parsecmd将命令字符串转换为命令结构
                               // runcmd执行命令结构，不会返回
      wait(0);                 // 等待子进程结束 (Wait for Child Process to Finish)
                               // 参数0表示不关心子进程的退出状态
                               // 这确保了shell在执行完一个命令后才继续读取下一个命令
    }
  }
  exit(0); // 正常退出shell (Exit Shell Normally)
           // 当getcmd返回-1（用户输入Ctrl+D）时，执行到这里
           // 以状态码0退出，表示正常结束
}

// 错误处理函数 (Error Handling Function)
//
// 函数功能 (Function Description)：
// 这是一个简单的错误处理函数，用于在遇到严重错误时打印错误信息并退出程序。
// 它是shell中处理不可恢复错误的标准方式。
//
// 参数说明 (Parameter Description)：
// s - 指向错误信息字符串的指针 (pointer to error message string)
//     这个字符串应该包含描述错误的简短信息，会被打印到标准错误
//
// 返回值 (Return Value)：
// 无返回值 (No Return Value)
// 函数会调用exit(1)退出程序，不会返回到调用者
//
// 工作流程 (Workflow)：
// 1. 使用fprintf将错误信息打印到标准错误（文件描述符2）
// 2. 调用exit(1)以错误状态退出程序
//
// 使用场景 (Usage Scenarios)：
// - 命令解析失败时（如语法错误）
// - 系统调用失败时（如fork失败）
// - 遇到不可恢复的错误时
//
// 注意事项 (Notes)：
// - 错误信息输出到标准错误而不是标准输出，这样即使标准输出被重定向，错误信息仍然可见
// - 使用exit(1)表示程序异常退出，与正常退出时的exit(0)相区别
// - 函数不会返回，调用panic后的代码不会被执行
void panic(char *s)
{
  fprintf(2, "%s\n", s); // 将错误信息输出到标准错误（文件描述符2）
                         // 参数说明：
                         // 第一个参数2：文件描述符，指定标准错误
                         // 第二个参数"%s\n"：格式字符串，%s会被替换为错误信息，\n表示换行
                         // 第三个参数s：错误信息字符串
  exit(1);               // 以错误状态退出 (Exit with Error Status)
                         // 参数1表示程序异常退出，与正常退出时的0相区别
                         // 这会终止当前进程，并返回1给父进程
}

// 创建子进程的包装函数 (Fork Wrapper Function)
//
// 函数功能 (Function Description)：
// 这是一个fork系统调用的包装函数，用于创建子进程。
// 与直接调用fork不同，这个函数在失败时会调用panic函数报错退出，
// 而不是返回错误码，简化了错误处理。
//
// 参数说明 (Parameter Description)：
// 无参数 (No Parameters)
//
// 返回值 (Return Value)：
// 在父进程中返回子进程的PID（大于0）(In parent process, returns child's PID (> 0))
// 在子进程中返回0 (In child process, returns 0)
// 函数不会返回-1，因为如果fork失败，会调用panic退出程序
//
// 工作流程 (Workflow)：
// 1. 调用fork系统调用创建子进程
// 2. 检查fork的返回值
// 3. 如果返回-1（失败），调用panic函数报错退出
// 4. 否则，返回fork的返回值
//
// 错误处理 (Error Handling)：
// 如果fork系统调用失败（返回-1），函数会调用panic("fork")，
// 打印错误信息并退出程序，而不是返回错误码让调用者处理。
//
// 使用场景 (Usage Scenarios)：
// - 在shell中创建子进程执行命令时
// - 任何需要创建子进程且失败时应该终止程序的场景
//
// 注意事项 (Notes)：
// - 函数名fork1中的"1"可能表示这是第一个或主要的fork包装函数
// - 使用这个函数可以简化调用者的错误处理逻辑
// - 由于失败时会调用panic，调用者不需要检查返回值是否为-1
int fork1(void)
{
  int pid; // 进程ID变量 (Process ID Variable)
           // 用于存储fork系统调用的返回值

  pid = fork(); // 创建子进程 (Create Child Process)
                // fork系统调用创建一个与当前进程几乎相同的子进程
                // 在父进程中，返回子进程的PID（大于0）
                // 在子进程中，返回0
                // 如果失败，返回-1

  if (pid == -1)   // fork失败 (Fork Failed)
                   // 检查fork是否失败
                   // 失败的原因可能是系统资源不足、进程数达到上限等
    panic("fork"); // 调用panic函数报错退出
                   // 传递字符串"fork"作为错误信息
                   // panic会打印错误信息并调用exit(1)退出程序

  return pid; // 返回进程ID (Return Process ID)
              // 如果是父进程，返回子进程的PID
              // 如果是子进程，返回0
}

// PAGEBREAK!
//  命令构造函数 (Command Constructors)
//
//  这部分包含了一系列用于创建不同类型命令结构体的函数。
//  这些函数负责分配内存、初始化结构体字段，并返回一个通用的命令指针。
//  使用构造函数可以简化命令结构的创建过程，提高代码的可读性和可维护性。

// 执行命令构造函数 (Execute Command Constructor)
//
// 函数功能 (Function Description)：
// 创建并初始化一个执行命令结构体（struct execcmd）。
// 这种命令结构用于表示简单的命令执行，如"ls"、"echo hello"等。
//
// 参数说明 (Parameter Description)：
// 无参数 (No Parameters)
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，实际指向一个新创建的struct execcmd
// 调用者应该将返回值视为struct cmd*类型，以保持接口的一致性
//
// 工作流程 (Workflow)：
// 1. 使用malloc分配足够的内存来存储struct execcmd
// 2. 使用memset将分配的内存清零，确保所有字段都被初始化为0
// 3. 设置命令类型为EXEC
// 4. 将struct execcmd*转换为struct cmd*并返回
//
// 内存管理 (Memory Management)：
// - 函数使用malloc分配内存，调用者需要确保最终释放这些内存
// - 在xv6 shell中，这些命令结构通常在命令执行完成后被自动释放
//
// 注意事项 (Notes)：
// - 函数不检查malloc是否成功，在xv6环境中假设内存分配总是成功
// - 返回的命令结构体的argv和eargv数组都被初始化为NULL
struct cmd *
execcmd(void)
{
  struct execcmd *cmd; // 执行命令结构体指针 (Execute Command Structure Pointer)

  cmd = malloc(sizeof(*cmd)); // 分配内存 (Allocate Memory)
                              // sizeof(*cmd)计算struct execcmd的大小
                              // malloc分配足够的内存来存储这个结构体

  memset(cmd, 0, sizeof(*cmd)); // 清零内存 (Zero Memory)
                                // 将分配的内存区域全部设置为0
                                // 这确保了所有字段（包括指针数组）都被初始化为0

  cmd->type = EXEC; // 设置命令类型 (Set Command Type)
                    // 将type字段设置为EXEC，标识这是一个执行命令结构

  return (struct cmd *)cmd; // 返回通用命令指针 (Return Generic Command Pointer)
                            // 将struct execcmd*转换为struct cmd*
                            // 这样可以保持接口的一致性，所有命令构造函数都返回相同的类型
}

// 重定向命令构造函数 (Redirect Command Constructor)
//
// 函数功能 (Function Description)：
// 创建并初始化一个重定向命令结构体（struct redircmd）。
// 这种命令结构用于表示包含输入/输出重定向的命令，如"cat < input.txt"、"ls > output.txt"。
//
// 参数说明 (Parameter Description)：
// subcmd - 指向要执行的子命令的指针 (pointer to the subcommand to execute)
//         这个子命令可以是任何类型的命令，它将在重定向设置好后被执行
//
// file - 指向重定向目标文件名字符串的指针 (pointer to the redirect target filename string)
//       这个字符串应该包含要重定向到的文件的名称
//
// efile - 指向文件名字符串结束位置的指针 (pointer to the end of the filename string)
//        这个指针用于在nulterminate函数中正确终止文件名字符串
//
// mode - 文件打开模式 (file open mode)
//       指定文件的打开方式，如O_RDONLY、O_WRONLY、O_CREATE等
//       例如：对于">"操作符，mode为O_WRONLY|O_CREATE|O_TRUNC
//
// fd - 要重定向的文件描述符 (file descriptor to redirect)
//     指定要重定向的标准文件描述符：0（标准输入）、1（标准输出）或2（标准错误）
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，实际指向一个新创建的struct redircmd
//
// 工作流程 (Workflow)：
// 1. 分配并清零内存
// 2. 设置命令类型为REDIR
// 3. 设置各个字段为传入的参数值
// 4. 返回转换后的通用命令指针
//
// 注意事项 (Notes)：
// - 函数不检查参数的有效性，调用者需要确保参数正确
// - 文件名字符串不需要以空字符结尾，这将在nulterminate函数中处理
struct cmd *
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd; // 重定向命令结构体指针 (Redirect Command Structure Pointer)

  cmd = malloc(sizeof(*cmd));   // 分配内存 (Allocate Memory)
  memset(cmd, 0, sizeof(*cmd)); // 清零内存 (Zero Memory)
  cmd->type = REDIR;            // 设置命令类型 (Set Command Type)

  // 设置结构体字段 (Set Structure Fields)
  cmd->cmd = subcmd;  // 设置子命令 (Set Subcommand)
  cmd->file = file;   // 设置文件名 (Set Filename)
  cmd->efile = efile; // 设置文件名结束位置 (Set Filename End Position)
  cmd->mode = mode;   // 设置文件打开模式 (Set File Open Mode)
  cmd->fd = fd;       // 设置要重定向的文件描述符 (Set File Descriptor to Redirect)

  return (struct cmd *)cmd; // 返回通用命令指针 (Return Generic Command Pointer)
}

// 管道命令构造函数 (Pipe Command Constructor)
//
// 函数功能 (Function Description)：
// 创建并初始化一个管道命令结构体（struct pipecmd）。
// 这种命令结构用于表示通过管道连接的两个命令，如"ls | grep pattern"。
//
// 参数说明 (Parameter Description)：
// left - 指向管道左侧命令的指针 (pointer to the left command of the pipe)
//       这个命令的标准输出将被重定向到管道的写入端
//
// right - 指向管道右侧命令的指针 (pointer to the right command of the pipe)
//        这个命令的标准输入将从管道的读取端读取
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，实际指向一个新创建的struct pipecmd
//
// 工作流程 (Workflow)：
// 1. 分配并清零内存
// 2. 设置命令类型为PIPE
// 3. 设置left和right字段为传入的命令指针
// 4. 返回转换后的通用命令指针
//
// 注意事项 (Notes)：
// - 函数不检查参数是否为NULL，调用者需要确保参数有效
// - 管道的实际创建和连接在runcmd函数中处理
struct cmd *
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd; // 管道命令结构体指针 (Pipe Command Structure Pointer)

  cmd = malloc(sizeof(*cmd));   // 分配内存 (Allocate Memory)
  memset(cmd, 0, sizeof(*cmd)); // 清零内存 (Zero Memory)
  cmd->type = PIPE;             // 设置命令类型 (Set Command Type)

  // 设置管道的左右命令 (Set Left and Right Commands of Pipe)
  cmd->left = left;   // 设置左侧命令（写入端）(Set Left Command - Write End)
  cmd->right = right; // 设置右侧命令（读取端）(Set Right Command - Read End)

  return (struct cmd *)cmd; // 返回通用命令指针 (Return Generic Command Pointer)
}

// 命令列表构造函数 (Command List Constructor)
//
// 函数功能 (Function Description)：
// 创建并初始化一个命令列表结构体（struct listcmd）。
// 这种命令结构用于表示按顺序执行的多个命令，如"ls; echo done"。
//
// 参数说明 (Parameter Description)：
// left - 指向第一个要执行的命令的指针 (pointer to the first command to execute)
//       这个命令将首先被执行
//
// right - 指向第二个要执行的命令的指针 (pointer to the second command to execute)
//        这个命令将在第一个命令完成后被执行
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，实际指向一个新创建的struct listcmd
//
// 工作流程 (Workflow)：
// 1. 分配并清零内存
// 2. 设置命令类型为LIST
// 3. 设置left和right字段为传入的命令指针
// 4. 返回转换后的通用命令指针
//
// 注意事项 (Notes)：
// - 函数不检查参数是否为NULL，调用者需要确保参数有效
// - 命令的实际顺序执行在runcmd函数中处理
struct cmd *
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd; // 命令列表结构体指针 (Command List Structure Pointer)

  cmd = malloc(sizeof(*cmd));   // 分配内存 (Allocate Memory)
  memset(cmd, 0, sizeof(*cmd)); // 清零内存 (Zero Memory)
  cmd->type = LIST;             // 设置命令类型 (Set Command Type)

  // 设置命令列表的左右命令 (Set Left and Right Commands of List)
  cmd->left = left;   // 设置第一个要执行的命令 (Set First Command to Execute)
  cmd->right = right; // 设置第二个要执行的命令 (Set Second Command to Execute)

  return (struct cmd *)cmd; // 返回通用命令指针 (Return Generic Command Pointer)
}

// 后台命令构造函数 (Background Command Constructor)
//
// 函数功能 (Function Description)：
// 创建并初始化一个后台命令结构体（struct backcmd）。
// 这种命令结构用于表示在后台执行的命令，如"sleep 10 &"。
//
// 参数说明 (Parameter Description)：
// subcmd - 指向要在后台执行的命令的指针 (pointer to the command to execute in background)
//         这个命令可以是任何类型的命令，它将在后台被执行
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，实际指向一个新创建的struct backcmd
//
// 工作流程 (Workflow)：
// 1. 分配并清零内存
// 2. 设置命令类型为BACK
// 3. 设置cmd字段为传入的子命令指针
// 4. 返回转换后的通用命令指针
//
// 注意事项 (Notes)：
// - 函数不检查参数是否为NULL，调用者需要确保参数有效
// - 后台执行的实际处理在runcmd函数中完成
struct cmd *
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd; // 后台命令结构体指针 (Background Command Structure Pointer)

  cmd = malloc(sizeof(*cmd));   // 分配内存 (Allocate Memory)
  memset(cmd, 0, sizeof(*cmd)); // 清零内存 (Zero Memory)
  cmd->type = BACK;             // 设置命令类型 (Set Command Type)

  cmd->cmd = subcmd; // 设置要在后台执行的命令 (Set Command to Execute in Background)

  return (struct cmd *)cmd; // 返回通用命令指针 (Return Generic Command Pointer)
}
// PAGEBREAK!
//  命令解析部分 (Command Parsing Section)
//
//  这部分包含了一系列用于解析用户输入的命令字符串的函数。
//  解析过程将命令字符串转换为内部的命令结构体表示，以便后续执行。
//  解析器支持各种shell特性，包括重定向、管道、命令列表和后台执行。

// 全局字符数组定义 (Global Character Array Definitions)
// 这些数组定义了在命令解析过程中需要识别的特殊字符

char whitespace[] = " \t\r\n\v"; // 空白字符数组 (Whitespace Characters Array)
                                 // 包含所有被视为空白字符的字符：
                                 // ' ' - 空格 (space)
                                 // '\t' - 水平制表符 (horizontal tab)
                                 // '\r' - 回车符 (carriage return)
                                 // '\n' - 换行符 (newline)
                                 // '\v' - 垂直制表符 (vertical tab)
                                 // 这些字符在命令解析中被忽略，用于分隔命令和参数

char symbols[] = "<|>&;()"; // 特殊符号数组 (Special Symbols Array)
                            // 包含所有在shell中有特殊含义的符号：
                            // '<' - 输入重定向 (input redirection)
                            // '|' - 管道 (pipe)
                            // '>' - 输出重定向 (output redirection)
                            // '&' - 后台执行 (background execution)
                            // ';' - 命令分隔符 (command separator)
                            // '(' 和 ')' - 子命令分组 (subcommand grouping)
                            // 这些符号在命令解析中被特殊处理

// 获取下一个标记 (Get Next Token)
//
// 函数功能 (Function Description)：
// 从命令字符串中提取下一个标记（token），并更新字符串指针的位置。
// 标记可以是特殊符号、单词或字符串结束符。
//
// 参数说明 (Parameter Description)：
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     这是一个双重指针，函数会修改它指向的指针值，使其跳过已解析的部分
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止解析越界
//
// q - 指向标记开始位置的指针的指针 (pointer to pointer to token start position)
//    如果不为NULL，函数会设置它指向标记的开始位置
//
// eq - 指向标记结束位置的指针的指针 (pointer to pointer to token end position)
//     如果不为NULL，函数会设置它指向标记的结束位置
//
// 返回值 (Return Value)：
// 返回一个字符，表示标记的类型：
// - 'a'：普通单词（如命令名、参数等）
// - 特殊符号本身：如'<', '>', '|', ';', '&', '(', ')'
// - '+'：表示">>"（追加输出重定向）
// - 0：表示字符串结束
//
// 工作流程 (Workflow)：
// 1. 跳过开头的空白字符
// 2. 记录标记的开始位置（如果q不为NULL）
// 3. 根据当前字符确定标记类型
// 4. 对于特殊符号，只取一个字符（除了">>"的情况）
// 5. 对于普通单词，读取直到遇到空白字符或特殊符号
// 6. 记录标记的结束位置（如果eq不为NULL）
// 7. 跳过标记后的空白字符
// 8. 更新字符串指针的位置
// 9. 返回标记类型
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其指向下一个未解析的字符
// - 标记不包括前导和尾随的空白字符
// - 对于普通单词，标记包括从开始到结束的所有字符（不包括空白和特殊符号）
int gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s; // 当前字符指针 (Current Character Pointer)
           // 用于遍历字符串的临时指针
  int ret; // 返回值 (Return Value)
           // 用于存储标记类型的字符

  s = *ps; // 初始化当前指针为字符串指针的当前值
           // 从当前位置开始解析

  // 跳过开头的空白字符 (Skip Leading Whitespace)
  // 循环直到遇到非空白字符或到达字符串末尾
  // strchr(whitespace, *s)检查当前字符是否是空白字符
  while (s < es && strchr(whitespace, *s))
    s++;

  // 记录标记的开始位置 (Record Token Start Position)
  // 如果调用者提供了q指针，设置它指向标记的开始位置
  if (q)
    *q = s;

  // 确定标记类型 (Determine Token Type)
  // 根据当前字符设置返回值
  ret = *s; // 由于ret是int类型，会存储字符的ASCII码值

  // 根据当前字符进行不同的处理 (Process Based on Current Character)
  switch (*s)
  {
  case 0:  // 字符串结束符 (String Terminator)
    break; // 不移动指针，直接返回0

  case '|': // 管道符号 (Pipe Symbol)
  case '(': // 左括号 (Left Parenthesis)
  case ')': // 右括号 (Right Parenthesis)
  case ';': // 命令分隔符 (Command Separator)
  case '&': // 后台执行符号 (Background Execution Symbol)
  case '<': // 输入重定向符号 (Input Redirection Symbol)
    s++;    // 移动到下一个字符
    break;

  case '>': // 输出重定向符号 (Output Redirection Symbol)
    s++;    // 移动到下一个字符
    // 检查是否是">>"（追加输出重定向）
    if (*s == '>')
    {
      ret = '+'; // 使用'+'表示">>"
      s++;       // 再移动一个字符
    }
    break;

  default:     // 普通单词 (Ordinary Word)
    ret = 'a'; // 使用'a'表示普通单词
    // 读取直到遇到空白字符或特殊符号
    while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }

  // 记录标记的结束位置 (Record Token End Position)
  // 如果调用者提供了eq指针，设置它指向标记的结束位置
  if (eq)
    *eq = s;

  // 跳过标记后的空白字符 (Skip Trailing Whitespace)
  // 循环直到遇到非空白字符或到达字符串末尾
  while (s < es && strchr(whitespace, *s))
    s++;

  // 更新字符串指针的位置 (Update String Pointer Position)
  // 将原始指针更新为当前位置，以便下次解析从正确的位置开始
  *ps = s;

  // 返回标记类型 (Return Token Type)
  return ret;
}

// 查看下一个标记 (Peek at Next Token)
//
// 函数功能 (Function Description)：
// 查看字符串中的下一个非空白字符是否是指定的字符之一。
// 这个函数不会消耗字符（即不会移动字符串指针），只是进行检查。
//
// 参数说明 (Parameter Description)：
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     函数会修改它指向的指针值，使其跳过空白字符，但不会跳过非空白字符
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止检查越界
//
// toks - 包含目标字符的字符串 (string containing target characters)
//       函数会检查下一个非空白字符是否在这个字符串中
//
// 返回值 (Return Value)：
// 非零值（true）：如果下一个非空白字符在toks字符串中
// 0（false）：如果下一个非空白字符不在toks字符串中，或者已经到达字符串末尾
//
// 工作流程 (Workflow)：
// 1. 从当前位置开始，跳过所有空白字符
// 2. 更新字符串指针，使其指向第一个非空白字符
// 3. 检查这个字符是否在toks字符串中
// 4. 返回检查结果
//
// 使用场景 (Usage Scenarios)：
// - 在解析命令时，检查是否遇到特定的操作符（如|、;、&等）
// - 在解析重定向时，检查是否遇到<或>符号
// - 在解析参数时，检查是否遇到命令结束符
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其跳过空白字符，但不会跳过非空白字符
// - 函数不会消耗字符，即不会移动指针越过检查的字符
// - 如果已经到达字符串末尾，函数返回0
int peek(char **ps, char *es, char *toks)
{
  char *s; // 当前字符指针 (Current Character Pointer)
           // 用于遍历字符串的临时指针

  s = *ps; // 初始化当前指针为字符串指针的当前值
           // 从当前位置开始检查

  // 跳过空白字符 (Skip Whitespace Characters)
  // 循环直到遇到非空白字符或到达字符串末尾
  // strchr(whitespace, *s)检查当前字符是否是空白字符
  while (s < es && strchr(whitespace, *s))
    s++;

  // 更新字符串指针的位置 (Update String Pointer Position)
  // 将原始指针更新为第一个非空白字符的位置
  // 这样下次解析时就不需要再次跳过这些空白字符
  *ps = s;

  // 检查当前字符是否在目标字符集中 (Check if Current Character is in Target Set)
  // *s && strchr(toks, *s) 的含义：
  // - *s：检查是否已经到达字符串末尾（*s为0表示字符串结束）
  // - strchr(toks, *s)：检查当前字符是否在toks字符串中
  // 如果两个条件都满足，返回非零值（true），否则返回0（false）
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

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
struct cmd *
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
  if (s != es)
  {
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
struct cmd *
parseline(char **ps, char *es)
{
  struct cmd *cmd; // 解析后的命令结构体指针

  // 首先调用parsepipe (parse pipe) 函数解析可能的管道命令
  // 管道操作符（|）的优先级高于后台执行和命令列表
  // 例如：在"ls | grep x &"中，先解析"ls | grep x"，再处理后台执行
  cmd = parsepipe(ps, es);

  // 检查是否有后台执行操作符（&）
  // peek (peek) 函数检查当前位置是否是"&"字符
  // 使用while循环可以处理多个连续的&操作符（虽然这在语法上不太常见）
  while (peek(ps, es, "&"))
  {
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
  if (peek(ps, es, ";"))
  {
    // 使用gettoken函数消耗掉";"字符
    gettoken(ps, es, 0, 0);
    // 递归调用parseline解析分号后的命令
    // listcmd (list command) 创建一个listcmd结构，将当前命令和后续命令作为左右子命令
    cmd = listcmd(cmd, parseline(ps, es));
  }

  // 返回最终构建的命令结构
  return cmd;
}

// 解析管道命令 (Parse Pipe Command)
//
// 函数功能 (Function Description)：
// 解析可能包含管道操作符（|）的命令。
// 管道操作符用于将一个命令的输出连接到另一个命令的输入。
//
// 参数说明 (Parameter Description)：
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     函数会修改它指向的指针值，使其跳过已解析的部分
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止解析越界
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，表示解析后的命令结构：
// - 如果没有管道操作符，返回parseexec解析的命令（通常是execcmd）
// - 如果有管道操作符，返回一个pipecmd结构，包含左右两个命令
//
// 工作流程 (Workflow)：
// 1. 首先调用parseexec解析一个简单的执行命令
// 2. 检查是否有管道操作符（|）
// 3. 如果有，消耗掉管道操作符，并递归调用parsepipe解析右侧命令
// 4. 使用pipecmd将左右命令连接起来
// 5. 返回最终构建的命令结构
//
// 递归解析 (Recursive Parsing)：
// 函数使用递归来处理多个管道操作符，例如"cmd1 | cmd2 | cmd3"：
// - 首先解析cmd1
// - 然后递归解析"cmd2 | cmd3"
// - 在递归调用中，首先解析cmd2
// - 然后再次递归解析cmd3
// - 最终构建的命令结构为：pipecmd(cmd1, pipecmd(cmd2, cmd3))
//
// 解析优先级 (Parsing Priority)：
// 管道操作符的优先级高于后台执行（&）和命令列表（;），
// 但低于重定向操作符和括号分组。
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其跳过已解析的部分
// - 函数使用递归调用来处理多个连续的管道操作符
// - 函数依赖于parseexec、peek、gettoken和pipecmd等辅助函数
struct cmd *
parsepipe(char **ps, char *es)
{
  struct cmd *cmd; // 命令结构体指针 (Command Structure Pointer)
                   // 用于存储解析后的命令结构

  // 首先解析一个简单的执行命令 (First Parse a Simple Execute Command)
  // parseexec函数解析一个可能包含重定向的简单命令
  // 例如：在"ls | grep x"中，首先解析"ls"
  cmd = parseexec(ps, es);

  // 检查是否有管道操作符 (Check for Pipe Operator)
  // peek函数检查下一个非空白字符是否是"|"
  if (peek(ps, es, "|"))
  {
    // 消耗掉管道操作符 (Consume Pipe Operator)
    // gettoken函数跳过"|"字符，移动字符串指针
    // 参数0,0表示我们不关心token的具体内容，只是跳过它
    gettoken(ps, es, 0, 0);

    // 递归解析右侧命令并创建管道命令 (Recursively Parse Right Command and Create Pipe Command)
    // parsepipe(ps, es)递归解析管道右侧的命令
    // 例如：在"ls | grep x"中，递归解析"grep x"
    // pipecmd(cmd, parsepipe(ps, es))创建一个管道命令结构
    // 将左侧命令和右侧命令连接起来
    //    pipe
    //   /    \
    // A      pipe
    //       /    \
    //      B      C
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }

  // 返回解析后的命令结构 (Return Parsed Command Structure)
  // 如果没有管道操作符，返回parseexec解析的命令
  // 如果有管道操作符，返回pipecmd创建的管道命令结构
  return cmd;
}

// 解析重定向 (Parse Redirections)
//
// 函数功能 (Function Description)：
// 解析并处理命令中的输入/输出重定向操作符。
// 函数会检查并处理所有连续的重定向操作符，为每个重定向创建相应的重定向命令结构。
//
// 参数说明 (Parameter Description)：
// cmd - 指向当前命令结构的指针 (pointer to current command structure)
//      这个命令将被包装在重定向命令结构中
//
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     函数会修改它指向的指针值，使其跳过已解析的重定向部分
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止解析越界
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针：
// - 如果没有重定向操作符，返回原始的cmd
// - 如果有重定向操作符，返回一个redircmd结构，包装原始命令
// - 如果有多个重定向操作符，返回嵌套的redircmd结构
//
// 工作流程 (Workflow)：
// 1. 循环检查是否有重定向操作符（<、>、>>）
// 2. 对于每个重定向操作符：
//    a. 消耗掉重定向操作符
//    b. 获取重定向目标文件名
//    c. 根据重定向类型创建相应的重定向命令结构
//    d. 将原始命令包装在重定向命令结构中
// 3. 返回最终构建的命令结构
//
// 重定向类型 (Redirection Types)：
// - '<'：输入重定向，从文件读取输入
// - '>'：输出重定向，将输出写入文件（覆盖现有内容）
// - '>>'：追加输出重定向，将输出追加到文件末尾
//
// 嵌套重定向 (Nested Redirections)：
// 函数支持多个重定向操作符，例如"cmd < input > output"：
// - 首先处理输入重定向，创建redircmd(cmd, "input", O_RDONLY, 0)
// - 然后处理输出重定向，创建redircmd(redircmd(cmd, "input", O_RDONLY, 0), "output", O_WRONLY|O_CREATE|O_TRUNC, 1)
// - 最终形成嵌套的重定向命令结构
//
// 错误处理 (Error Handling)：
// 如果重定向操作符后没有文件名，函数会调用panic("missing file for redirection")报错退出。
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其跳过已解析的重定向部分
// - 函数使用while循环处理多个连续的重定向操作符
// - 函数依赖于peek、gettoken和redircmd等辅助函数
struct cmd *
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok; // 标记类型 (Token Type)
           // 用于存储重定向操作符的类型：'<'、'>'或'+'

  char *q, *eq; // 文件名位置指针 (Filename Position Pointers)
                // q指向文件名的开始位置
                // eq指向文件名的结束位置

  // 循环处理所有连续的重定向操作符 (Loop Through All Consecutive Redirection Operators)
  // peek函数检查下一个非空白字符是否是重定向操作符（<或>）
  while (peek(ps, es, "<>"))
  {
    // 获取重定向操作符 (Get Redirection Operator)
    // gettoken函数消耗掉重定向操作符，并返回其类型
    // 参数0,0表示我们不关心token的具体内容，只是跳过它
    tok = gettoken(ps, es, 0, 0);

    // 获取重定向目标文件名 (Get Redirection Target Filename)
    // gettoken函数获取下一个标记，应该是文件名
    // 如果返回值不是'a'（普通单词），说明没有文件名
    if (gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection"); // 报错退出：缺少重定向文件名

    // 根据重定向类型创建相应的重定向命令结构 (Create Redirection Command Based on Type)
    switch (tok)
    {
    case '<': // 输入重定向 (Input Redirection)
      // 创建输入重定向命令结构
      // 参数说明：
      // cmd - 原始命令
      // q, eq - 文件名和结束位置
      // O_RDONLY - 以只读方式打开文件
      // 0 - 重定向标准输入（文件描述符0）
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;

    case '>': // 输出重定向（覆盖）(Output Redirection - Overwrite)
      // 创建输出重定向命令结构
      // 参数说明：
      // cmd - 原始命令
      // q, eq - 文件名和结束位置
      // O_WRONLY|O_CREATE|O_TRUNC - 以只写方式打开文件，如果文件不存在则创建，如果文件存在则截断
      // 1 - 重定向标准输出（文件描述符1）
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE | O_TRUNC, 1);
      break;

    case '+': // 追加输出重定向 (Append Output Redirection)
      // '+'表示">>"操作符
      // 创建追加输出重定向命令结构
      // 参数说明：
      // cmd - 原始命令
      // q, eq - 文件名和结束位置
      // O_WRONLY|O_CREATE - 以只写方式打开文件，如果文件不存在则创建，但不截断现有内容
      // 1 - 重定向标准输出（文件描述符1）
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE, 1);
      break;
    }
  }

  // 返回处理后的命令结构 (Return Processed Command Structure)
  // 如果没有重定向操作符，返回原始的cmd
  // 如果有重定向操作符，返回包装后的redircmd结构
  return cmd;
}

// 解析块命令 (Parse Block Command)
//
// 函数功能 (Function Description)：
// 解析用括号括起来的命令块。
// 命令块允许将多个命令组合在一起，作为一个整体进行处理，
// 通常用于改变命令的执行优先级或组合多个命令。
//
// 参数说明 (Parameter Description)：
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     函数会修改它指向的指针值，使其跳过已解析的块命令部分
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止解析越界
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，表示解析后的块命令：
// - 块内可能包含任何类型的命令，包括简单命令、管道、列表等
// - 块命令后可能跟着重定向操作符，这些重定向会应用于整个块
//
// 工作流程 (Workflow)：
// 1. 检查是否有左括号'('，如果没有则报错
// 2. 消耗掉左括号
// 3. 调用parseline解析括号内的命令
// 4. 检查是否有右括号')'，如果没有则报错
// 5. 消耗掉右括号
// 6. 解析可能跟随在块命令后的重定向操作符
// 7. 返回最终构建的命令结构
//
// 使用场景 (Usage Scenarios)：
// - 改变执行优先级：例如"cmd1 ; cmd2 | cmd3"与"(cmd1 ; cmd2) | cmd3"的执行顺序不同
// - 组合多个命令：例如"(ls -l; pwd) > output"将两个命令的输出都重定向到同一文件
// - 子shell执行：括号内的命令会在子shell中执行，不会影响当前shell的状态
//
// 错误处理 (Error Handling)：
// - 如果缺少左括号，调用panic("parseblock")报错退出
// - 如果缺少右括号，调用panic("syntax - missing )")报错退出
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其跳过已解析的块命令部分
// - 块命令后可以跟重定向操作符，这些重定向会应用于整个块
// - 函数依赖于peek、gettoken、parseline和parseredirs等辅助函数
struct cmd *
parseblock(char **ps, char *es)
{
  struct cmd *cmd; // 命令结构体指针 (Command Structure Pointer)
                   // 用于存储解析后的块命令结构

  // 检查是否有左括号 (Check for Left Parenthesis)
  // peek函数检查下一个非空白字符是否是"("
  // 如果没有左括号，说明这不是一个块命令，报错退出
  if (!peek(ps, es, "("))
    panic("parseblock"); // 报错退出：解析块命令失败

  // 消耗掉左括号 (Consume Left Parenthesis)
  // gettoken函数跳过"("字符，移动字符串指针
  // 参数0,0表示我们不关心token的具体内容，只是跳过它
  gettoken(ps, es, 0, 0);

  // 解析括号内的命令 (Parse Command Inside Parentheses)
  // parseline函数解析括号内的命令，可以是任何类型的命令
  // 包括简单命令、管道、列表、后台执行等
  cmd = parseline(ps, es);

  // 检查是否有右括号 (Check for Right Parenthesis)
  // peek函数检查下一个非空白字符是否是")"
  // 如果没有右括号，说明语法错误，报错退出
  if (!peek(ps, es, ")"))
    panic("syntax - missing )"); // 报错退出：缺少右括号

  // 消耗掉右括号 (Consume Right Parenthesis)
  // gettoken函数跳过")"字符，移动字符串指针
  gettoken(ps, es, 0, 0);

  // 解析可能跟随在块命令后的重定向操作符 (Parse Redirections After Block)
  // 块命令后可以跟重定向操作符，这些重定向会应用于整个块
  // 例如："(ls; pwd) > output"将两个命令的输出都重定向到output文件
  cmd = parseredirs(cmd, ps, es);

  // 返回解析后的块命令结构 (Return Parsed Block Command Structure)
  return cmd;
}

// 解析执行命令 (Parse Execute Command)
//
// 函数功能 (Function Description)：
// 解析一个简单的执行命令，可能包含命令名、参数和重定向。
// 这是命令解析的底层函数，负责解析最基本的命令单元。
//
// 参数说明 (Parameter Description)：
// ps - 指向字符串指针的指针 (pointer to string pointer)
//     函数会修改它指向的指针值，使其跳过已解析的命令部分
//
// es - 字符串结束指针 (end of string pointer)
//     指向输入字符串的末尾，用于防止解析越界
//
// 返回值 (Return Value)：
// 返回一个指向struct cmd的指针，通常是execcmd结构：
// - 如果命令包含重定向，返回包装在redircmd结构中的execcmd
// - 如果命令以括号开头，返回parseblock解析的块命令
//
// 工作流程 (Workflow)：
// 1. 检查是否以左括号开头，如果是则调用parseblock解析块命令
// 2. 创建一个新的execcmd结构
// 3. 解析可能的重定向操作符
// 4. 循环解析命令名和参数，直到遇到命令结束符
// 5. 在每个参数后，检查是否有重定向操作符
// 6. 确保参数数量不超过MAXARGS限制
// 7. 以NULL结尾argv和eargv数组
// 8. 返回构建的命令结构
//
// 命令结束符 (Command Terminators)：
// 函数在遇到以下字符时停止解析参数：
// - '|'：管道操作符
// - ')'：右括号
// - '&'：后台执行操作符
// - ';'：命令分隔符
//
// 错误处理 (Error Handling)：
// - 如果遇到非单词标记（非'a'），调用panic("syntax")报错退出
// - 如果参数数量超过MAXARGS，调用panic("too many args")报错退出
//
// 注意事项 (Notes)：
// - 函数会修改*ps的值，使其跳过已解析的命令部分
// - 重定向可以出现在命令的任何位置，例如"cmd > file arg1 arg2"或"cmd arg1 > file arg2"
// - 函数依赖于peek、gettoken、parseblock、parseredirs和execcmd等辅助函数
struct cmd *
parseexec(char **ps, char *es)
{
  char *q, *eq; // 参数位置指针 (Argument Position Pointers)
                // q指向参数的开始位置
                // eq指向参数的结束位置

  int tok,  // 标记类型 (Token Type)
      argc; // 参数计数器 (Argument Counter)
            // 用于统计当前命令的参数数量，包括命令名本身

  struct execcmd *cmd; // 执行命令结构体指针 (Execute Command Structure Pointer)
                       // 指向新创建的execcmd结构

  struct cmd *ret; // 通用命令结构体指针 (Generic Command Structure Pointer)
                   // 用于返回的命令结构，可能是execcmd或包装在redircmd中的execcmd，ret是返回值，相当于就是用接口隐藏具体的实现

  // 检查是否以左括号开头 (Check if Starts with Left Parenthesis)
  // 如果命令以左括号开头，说明这是一个块命令，需要调用parseblock解析
  if (peek(ps, es, "("))
    return parseblock(ps, es);

  // 创建新的执行命令结构 (Create New Execute Command Structure)
  // execcmd函数分配并初始化一个execcmd结构
  ret = execcmd();

  // 将通用命令指针转换为执行命令指针 (Cast Generic Command Pointer to Execute Command Pointer)
  // 这样可以访问execcmd结构的特定字段，如argv和eargv
  cmd = (struct execcmd *)ret;

  // 初始化参数计数器 (Initialize Argument Counter)
  argc = 0;

  // 解析可能的重定向操作符 (Parse Possible Redirection Operators)
  // 重定向可以出现在命令的开头，例如"> file cmd"
  ret = parseredirs(ret, ps, es);

  // 循环解析命令名和参数 (Loop to Parse Command Name and Arguments)
  // 继续解析直到遇到命令结束符（|、)、&、;）
  while (!peek(ps, es, "|)&;"))
  {
    // 获取下一个标记 (Get Next Token)
    // 如果返回0，说明已经到达字符串末尾，退出循环
    if ((tok = gettoken(ps, es, &q, &eq)) == 0)
      break;

    // 检查标记类型 (Check Token Type)
    // 如果不是普通单词（'a'），说明遇到了语法错误
    if (tok != 'a')
      panic("syntax"); // 报错退出：语法错误

    // 存储参数位置 (Store Argument Position)
    // 将参数的开始和结束位置存储在argv和eargv数组中
    // 这些位置将在nulterminate函数中被用于正确终止字符串
    cmd->argv[argc] = q;   // 存储参数开始位置
    cmd->eargv[argc] = eq; // 存储参数结束位置

    // 增加参数计数器 (Increment Argument Counter)
    argc++;

    // 检查参数数量是否超过限制 (Check if Argument Count Exceeds Limit)
    // MAXARGS是命令参数的最大数量限制
    if (argc >= MAXARGS)
      panic("too many args"); // 报错退出：参数过多

    // 解析参数后可能的重定向操作符 (Parse Possible Redirection Operators After Argument)
    // 重定向可以出现在命令的任何位置，包括参数之间
    ret = parseredirs(ret, ps, es);
  }

  // 以NULL结尾参数数组 (Null-Terminate Argument Arrays)
  // argv和eargv数组必须以NULL指针结尾，这是exec系统调用的要求
  cmd->argv[argc] = 0;  // argv数组以NULL结尾
  cmd->eargv[argc] = 0; // eargv数组以NULL结尾

  // 返回构建的命令结构 (Return Constructed Command Structure)
  return ret;
}

// 空字符终止所有计数字符串 (Null-Terminate All Counted Strings)
//
// 函数功能 (Function Description)：
// 遍历命令结构中的所有字符串，并在它们的结束位置插入空字符（'\0'）。
// 这是命令解析的最后一步，确保所有字符串都被正确终止，以便后续执行。
//
// 参数说明 (Parameter Description)：
// cmd - 指向命令结构的指针 (pointer to command structure)
//      可以是任何类型的命令结构（execcmd、redircmd、pipecmd、listcmd、backcmd）
//
// 返回值 (Return Value)：
// 返回传入的命令结构指针（与输入参数相同）
// 这样允许函数调用链式使用，例如：cmd = nulterminate(parsecmd(s));
//
// 工作流程 (Workflow)：
// 1. 检查命令指针是否为NULL，如果是则直接返回
// 2. 根据命令类型进行相应的处理：
//    - EXEC：终止所有参数字符串
//    - REDIR：递归处理子命令并终止文件名字符串
//    - PIPE：递归处理左右两个命令
//    - LIST：递归处理左右两个命令
//    - BACK：递归处理子命令
// 3. 返回处理后的命令结构
//
// 为什么需要这个函数 (Why This Function is Needed)：
// 在命令解析过程中，字符串不会被立即复制，而是通过指针引用原始输入字符串。
// 解析器记录每个字符串的开始和结束位置，但不修改原始字符串。
// 在执行命令之前，需要确保所有字符串都被正确终止，因为C语言的标准库函数
// （如execve）期望字符串以空字符结尾。
//
// 处理方式 (Processing Method)：
// 函数通过在eargv、efile等指针指向的位置插入空字符来终止字符串。
// 这些位置是在解析过程中记录的字符串结束位置。
//
// 注意事项 (Notes)：
// - 函数会修改原始输入字符串，在适当位置插入空字符
// - 函数使用递归处理复合命令（如PIPE、LIST、BACK、REDIR）
// - 函数不会分配新的内存，只是修改现有字符串
struct cmd *
nulterminate(struct cmd *cmd)
{
  int i; // 循环计数器 (Loop Counter)
         // 用于遍历参数数组

  // 各种命令类型的指针 (Pointers for Different Command Types)
  // 这些指针用于在switch语句中根据命令类型进行类型转换
  struct backcmd *bcmd;  // 后台命令指针 (Background Command Pointer)
  struct execcmd *ecmd;  // 执行命令指针 (Execute Command Pointer)
  struct listcmd *lcmd;  // 命令列表指针 (List Command Pointer)
  struct pipecmd *pcmd;  // 管道命令指针 (Pipe Command Pointer)
  struct redircmd *rcmd; // 重定向命令指针 (Redirect Command Pointer)

  // 检查命令指针是否为NULL (Check if Command Pointer is NULL)
  // 如果命令指针为NULL，直接返回NULL
  if (cmd == 0)
    return 0;

  // 根据命令类型进行相应的处理 (Process Based on Command Type)
  // 使用switch语句根据命令类型进行分发处理
  switch (cmd->type)
  {
  case EXEC: // 处理执行命令 (Handle Execute Command)
    // 类型转换，将通用命令指针转换为执行命令指针
    ecmd = (struct execcmd *)cmd;

    // 遍历所有参数并终止字符串 (Iterate Through All Arguments and Terminate Strings)
    // argv数组以NULL指针结尾，所以循环直到遇到NULL
    for (i = 0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0; // 在参数结束位置插入空字符
                           // eargv[i]指向参数字符串的结束位置
                           // 在这个位置插入空字符，确保字符串正确终止
    break;

  case REDIR: // 处理重定向命令 (Handle Redirect Command)
    // 类型转换，将通用命令指针转换为重定向命令指针
    rcmd = (struct redircmd *)cmd;

    // 递归处理子命令 (Recursively Process Subcommand)
    // 重定向命令包含一个子命令，需要递归处理
    nulterminate(rcmd->cmd);

    // 终止文件名字符串 (Terminate Filename String)
    *rcmd->efile = 0; // 在文件名结束位置插入空字符
                      // efile指向文件名字符串的结束位置
                      // 在这个位置插入空字符，确保文件名正确终止
    break;

  case PIPE: // 处理管道命令 (Handle Pipe Command)
    // 类型转换，将通用命令指针转换为管道命令指针
    pcmd = (struct pipecmd *)cmd;

    // 递归处理左右两个命令 (Recursively Process Left and Right Commands)
    // 管道命令包含左右两个子命令，都需要递归处理
    nulterminate(pcmd->left);  // 处理左侧命令
    nulterminate(pcmd->right); // 处理右侧命令
    break;

  case LIST: // 处理命令列表 (Handle Command List)
    // 类型转换，将通用命令指针转换为命令列表指针
    lcmd = (struct listcmd *)cmd;

    // 递归处理左右两个命令 (Recursively Process Left and Right Commands)
    // 命令列表包含左右两个子命令，都需要递归处理
    nulterminate(lcmd->left);  // 处理左侧命令
    nulterminate(lcmd->right); // 处理右侧命令
    break;

  case BACK: // 处理后台命令 (Handle Background Command)
    // 类型转换，将通用命令指针转换为后台命令指针
    bcmd = (struct backcmd *)cmd;

    // 递归处理子命令 (Recursively Process Subcommand)
    // 后台命令包含一个子命令，需要递归处理
    nulterminate(bcmd->cmd);
    break;
  }

  // 返回处理后的命令结构 (Return Processed Command Structure)
  // 返回传入的命令结构指针，允许函数调用链式使用
  return cmd;
}
