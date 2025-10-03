# 测试文件链接

这是一个测试文件，用于验证 VSCode 的文件链接功能。

## 相对路径链接测试

1. 链接到 user/sh.c 的第 160 行：[user/sh.c:160](user/sh.c:160)
2. 链接到 kernel/sysfile.c 的第 435 行：[kernel/sysfile.c:435](kernel/sysfile.c:435)
3. 链接到 kernel/file.c 的第 135 行：[kernel/file.c:135](kernel/file.c:135)

## 绝对路径链接测试

1. 链接到 user/sh.c 的第 334 行：[/Users/Zhuanz/Projects/xv6-riscv/user/sh.c:334](/Users/Zhuanz/Projects/xv6-riscv/user/sh.c:334)
2. 链接到 kernel/sysfile.c 的第 83 行：[/Users/Zhuanz/Projects/xv6-riscv/kernel/sysfile.c:83](/Users/Zhuanz/Projects/xv6-riscv/kernel/sysfile.c:83)

## 直接路径格式测试

1. 直接路径 user/sh.c:160
2. 直接路径 kernel/sysfile.c:435
3. 直接路径 kernel/file.c:135

## 代码中的链接

```c
// 参见 user/sh.c:160 中的实现
// 参见 kernel/sysfile.c:435 中的实现
```

点击上述链接应该能够跳转到相应的文件和行号。