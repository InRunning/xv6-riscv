#define SBRK_ERROR ((char *)-1)

struct stat;
// LINK kernel/syscall.c
// system calls 系统调用，会跳转到kernel/syscall.c中的sys_xxx函数
// [sys_calls](sys_calls)
// [fork](sys_fork)
int fork(void);
// [exit](sys_exit)
int exit(int) __attribute__((noreturn));
// [wait](sys_wait)
int wait(int *);
// [pipe](sys_pipe)
int pipe(int *);
// [write](sys_write)
int write(int, const void *, int);
// [read](sys_read)
int read(int, void *, int);
// [close](sys_close)
int close(int);
// [kill](sys_kill)
int kill(int);
// [exec](sys_exec)
int exec(const char *, char **);
// [open](sys_open)
int open(const char *, int);
// [mknod](sys_mknod)
int mknod(const char *, short, short);
// [unlink](sys_unlink)
int unlink(const char *);
// [fstat](sys_fstat)
int fstat(int fd, struct stat *);
// [link](sys_link)
int link(const char *, const char *);
// [mkdir](sys_mkdir)
int mkdir(const char *);
// [chdir](sys_chdir)
int chdir(const char *);
// [dup](sys_dup)
int dup(int);
// [getpid](sys_getpid)
int getpid(void);
// [sys_sbrk](sys_sbrk)
char *sys_sbrk(int, int);
// [pause](sys_pause)
int pause(int);
// [uptime](sys_uptime)
int uptime(void);

// ulib.c
int stat(const char *, struct stat *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char c);
int strcmp(const char *, const char *);
char *gets(char *, int max);
uint strlen(const char *);
void *memset(void *, int, uint);
int atoi(const char *);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char *sbrk(int);
char *sbrklazy(int);

// printf.c
void fprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void printf(const char *, ...) __attribute__((format(printf, 1, 2)));

// umalloc.c
void *malloc(uint);
void free(void *);
