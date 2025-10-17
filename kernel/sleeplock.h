// Long-term locks for processes (进程的长期锁)
struct sleeplock {  // Sleep Lock (睡眠锁) - 允许进程在等待锁时进入睡眠状态的自旋锁
  uint locked;       // locked (锁定状态) - Is the lock held? (锁是否被持有？)
  struct spinlock lk; // lk: Lock (锁) - spinlock protecting this sleep lock (保护此睡眠锁的自旋锁)
  
  // For debugging: (用于调试)
  char *name;        // name (名称) - Name of lock. (锁的名称)
  int pid;           // pid: Process ID (进程ID) - Process holding lock (持有锁的进程)
};

