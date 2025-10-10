# echo "hi" > x — Mermaid 图

下面给出多张 Mermaid 图，展示 `echo "hi" > x` 在 xv6 中从 Shell 解析、程序执行、系统调用到文件系统与日志提交的完整流程。每张图聚焦一个层面，便于分块理解与对照源码阅读。

## 1. 总览（从 Shell 到磁盘提交）

```mermaid
flowchart TD
  subgraph Shell
    A1[sh.c: main()<br/>读取命令] --> A2[parsecmd/parseline/parseexec<br/>parseredirs 构造 AST]
    A2 --> A3[AST: redircmd(exec="echo", file="x"<br/>mode=O_WRONLY|O_CREATE|O_TRUNC, fd=1)]
    A3 --> A4[runcmd(REDIR): close(1); open("x",flags)]
    A4 --> A5[runcmd(EXEC): exec("echo", argv)]
  end

  subgraph User_Prog[用户程序]
    B1[echo: write(1, "hi", 2);\nwrite(1, "\n", 1)]
  end

  subgraph Syscall[系统调用层]
    C1[sys_open]:::sys --> C2
    C2[sys_exec]:::sys --> B1
    B1 --> C3[sys_write]:::sys
  end

  subgraph FS[文件系统层]
    C3 --> D1{file->type?}
    D1 -- FD_INODE --> D2[filewrite]
    D2 --> D3[begin_op]
    D3 --> D4[ilock]
    D4 --> D5[writei: 分块写入]
    D5 --> D6[iupdate]
    D6 --> D7[iunlock]
    D7 --> D8[end_op]
  end

  subgraph Log[日志子系统]
    D5 --> E1[log_write(数据块...)]
    D6 --> E1
    E1 --> E2[end_op 触发 commit]
    E2 --> E3[commit: write_log → write_head → install_trans → 清空头]
  end

  subgraph Disk[块缓存/磁盘]
    D5 --> F1[bmap/bread/bwrite\n(通过 bio.c 与 virtio)]
    E3 --> F2[将日志区的数据\n落盘并回放到目标块]
  end

  classDef sys fill:#eef,stroke:#66f
```

## 2. Shell 解析与执行（重定向 + exec）

```mermaid
flowchart TD
  S1[main: getcmd] --> S2[parsecmd]
  S2 --> S3[parseline → parseexec → parseredirs]
  S3 --> S4[构造 redircmd(execcmd=echo, file=x, fd=1,\nmode=O_WRONLY|O_CREATE|O_TRUNC)]
  S4 --> S5[nulterminate 完成收尾]
  S5 --> S6[fork 子进程]
  S6 --> S7[runcmd(REDIR): close(1)]
  S7 --> S8[open("x", O_WRONLY|O_CREATE|O_TRUNC)]
  S8 --> S9[runcmd(EXEC): exec("echo", argv)]
```

## 3. write() 写路径（sys_write → filewrite → writei）

```mermaid
flowchart TD
  W0[echo 用户态: write(fd=1, buf, n)] --> W1[sys_write]
  W1 --> W2[argfd 解析 fd 得到 struct file]
  W2 --> W3{file->type}
  W3 -- FD_PIPE --> WP[pipewrite]
  W3 -- FD_DEVICE --> WD[devsw[major].write]
  W3 -- FD_INODE --> W4[filewrite]
  W4 --> W5[begin_op]
  W5 --> W6[ilock]
  W6 --> W7[writei]
  W7 --> W8[iupdate]
  W8 --> W9[iunlock]
  W9 --> W10[end_op]
```

## 4. writei 细节（分块映射与日志记录）

```mermaid
flowchart TD
  I0[writei(ip, src, off, n)] --> I1[参数校验与截断]
  I1 --> I2{还有待写字节?}
  I2 -- 是 --> I3[bmap(ip, LBA) 映射数据块]
  I3 --> I4[bread(dev, blkno) 取得缓存 buf]
  I4 --> I5[either_copyin 将用户数据拷入 buf]
  I5 --> I6[log_write(buf) 加入日志事务]
  I6 --> I7[brelse 释放 buf]
  I7 --> I2
  I2 -- 否 --> I8[若扩大文件: 更新 ip->size]
  I8 --> I9[iupdate 记录 inode 变化]
```

## 5. 日志事务与提交（begin_op → end_op/commit）

```mermaid
flowchart TD
  L1[begin_op] --> L2[获取 log.lock]
  L2 --> L3{空间/提交是否可用?}
  L3 -- 等待 --> L2
  L3 -- 通过 --> L4[log.outstanding++ 释放锁]

  subgraph 事务中
    L4 --> L5[log_write(buf): 去重并记录块号\n必要时 bpin 固定缓存]
  end

  L5 --> L6[end_op]
  L6 --> L7[获取 log.lock; log.outstanding--]
  L7 --> L8{归零?}
  L8 -- 否 --> L9[释放锁 返回]
  L8 -- 是 --> L10[设 committing=1; 释放锁]

  L10 --> LC[commit]
  LC --> C1{log.lh.n > 0?}
  C1 -- 否 --> C6[清空/写回头部(空) 结束]
  C1 -- 是 --> C2[write_log: 将缓存数据块\n顺序写入日志区域]
  C2 --> C3[write_head: 写日志头 = 提交]
  C3 --> C4[install_trans: 回放到目标块]
  C4 --> C5[清空 log.lh 并再次 write_head(清零)]
```

## 6. 文件创建（open O_CREATE → create）

```mermaid
flowchart TD
  O1[sys_open("x", O_CREATE|O_TRUNC|O_WRONLY)] --> O2{O_CREATE?}
  O2 -- 是 --> O3[create(path, T_FILE, ...)]
  O2 -- 否 --> O7[namei 查找既有 inode]

  O3 --> O4[nameiparent 找父目录 + 末级名]
  O4 --> O5{已存在同名?}
  O5 -- 是(文件) --> O6[复用 inode]
  O5 -- 否 --> O8[ialloc 分配 inode, 清零并 iupdate]
  O8 --> O9[若目录: 建立 . 与 ..]
  O9 --> O10[dirlink 将新 inode 写入父目录]
  O6 --> O11[filealloc/ fdalloc 建立 struct file 与 fd]
  O10 --> O11
```

—

使用方式：建议配合 `docs/echo执行流程.md` 的函数定位一起阅读，先看第 1 张“总览图”把握主线，再按需展开第 3～6 张图以理解关键路径（写入流程、日志、创建与块映射）。
