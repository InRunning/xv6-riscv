// Shell.
// 这是一个简单的 Unix shell 实现，支持基本的命令执行、重定向、管道和后台任务功能。

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// 命令类型常量定义
// Parsed command representation
#define EXEC  1  // 执行普通命令
#define REDIR 2  // 重定向命令
#define PIPE  3  // 管道命令
#define LIST  4  // 命令列表（顺序执行）
#define BACK  5  // 后台执行命令

#define MAXARGS 10  // 命令参数的最大数量

// 基础命令结构体，所有命令类型都继承自这个结构
struct cmd {
  int type;  // 命令类型（EXEC, REDIR, PIPE, LIST, BACK）
};

// 执行命令结构体
struct execcmd {
  int type;              // 命令类型，此处为 EXEC
  char *argv[MAXARGS];   // 命令参数数组
  char *eargv[MAXARGS];  // 参数结束位置指针数组，用于字符串终止
};

// 重定向命令结构体
struct redircmd {
  int type;         // 命令类型，此处为 REDIR
  struct cmd *cmd;  // 要执行的子命令
  char *file;       // 重定向目标文件名
  char *efile;      // 文件名结束位置指针，用于字符串终止
  int mode;         // 文件打开模式（读/写）
  int fd;           // 要重定向的文件描述符
};

// 管道命令结构体
struct pipecmd {
  int type;         // 命令类型，此处为 PIPE
  struct cmd *left;  // 管道左侧命令（写入端）
  struct cmd *right; // 管道右侧命令（读取端）
};

// 命令列表结构体（顺序执行多个命令）
struct listcmd {
  int type;         // 命令类型，此处为 LIST
  struct cmd *left;  // 第一个要执行的命令
  struct cmd *right; // 第二个要执行的命令
};

// 后台命令结构体
struct backcmd {
  int type;         // 命令类型，此处为 BACK
  struct cmd *cmd;  // 要在后台执行的命令
};

// 函数声明
int fork1(void);  // Fork but panics on failure. // 创建子进程，失败时调用panic
void panic(char*); // 错误处理函数，打印错误信息并退出
struct cmd *parsecmd(char*); // 解析命令字符串，生成命令结构体
void runcmd(struct cmd*) __attribute__((noreturn)); // 执行命令，永不返回

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
// buf: 存储命令的缓冲区
// nbuf: 缓冲区大小
// 返回值: 0表示成功获取命令，-1表示EOF（用户输入Ctrl+D）
int
getcmd(char *buf, int nbuf)
{
  write(2, "$ ", 2);  // 显示shell提示符
  memset(buf, 0, nbuf);  // 清空缓冲区
  gets(buf, nbuf);  // 读取用户输入
  if(buf[0] == 0) // EOF，用户输入Ctrl+D
    return -1;
  return 0;
}

// Shell主函数
int
main(void)
{
  static char buf[100];  // 命令缓冲区
  int fd;

  // 确保三个标准文件描述符（0,1,2）是打开的
  // 如果console设备文件被打开，会返回文件描述符
  // 我们需要确保0,1,2这三个描述符被占用，其他打开的描述符需要关闭
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){  // 如果文件描述符大于等于3，说明标准描述符已经打开
      close(fd);  // 关闭多余的描述符
      break;
    }
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

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
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
