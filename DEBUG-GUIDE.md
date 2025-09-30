# xv6-riscv VSCode 调试指南

## ✅ 环境验证

**测试脚本已验证调试环境完全可用！**

运行测试：
```bash
./test-debug.sh
```

测试结果：
- ✓ QEMU 10.1.0
- ✓ GDB 16.3
- ✓ kernel 包含调试信息
- ✓ GDB 可以成功连接 QEMU

---

## 🎯 VSCode 调试方法

### 方法一：自动启动（推荐）

1. **按 F5** 或点击"运行和调试"
2. 选择 `Debug xv6 Kernel`
3. 等待构建和 QEMU 启动
4. 开始调试

**注意：** QEMU 会打开一个图形窗口，这是正常的。

### 方法二：手动启动 QEMU

如果自动启动有问题，使用此方法：

#### 步骤 1: 启动 QEMU
在终端运行：
```bash
make kernel/kernel fs.img
qemu-system-riscv64 \
  -machine virt \
  -bios none \
  -kernel kernel/kernel \
  -m 128M \
  -smp 3 \
  -global virtio-mmio.force-legacy=false \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -s -S
```

#### 步骤 2: VSCode 连接
1. 按 F5
2. 选择 `Debug xv6 (Manual QEMU)`

---

## 📋 配置文件说明

### `.vscode/launch.json`
- **Debug xv6 Kernel**: 自动构建 + 启动 QEMU + 调试
- **Debug xv6 (Manual QEMU)**: 连接已运行的 QEMU

### `.vscode/tasks.json`
- **build-xv6**: 构建并启动 QEMU（调试模式）
- **build-kernel-only**: 仅构建 kernel
- **run-qemu**: 正常运行（无调试）
- **kill-qemu**: 停止 QEMU

### 关键参数
- **GDB 端口**: 1234（QEMU 默认）
- **QEMU 参数**: `-s -S`（监听 1234 + 启动暂停）
- **无 `-nographic`**: 避免 GDB stub 兼容性问题

---

## 🐛 故障排查

### 问题 1: "Configured debug type 'cppdbg' is not supported"
**解决：** 安装 VSCode C/C++ 扩展
```
扩展 ID: ms-vscode.cpptools
```

### 问题 2: "connection timed out"
**原因：** QEMU 未启动或端口未监听

**检查：**
```bash
# 检查 QEMU 进程
ps aux | grep qemu-system-riscv64

# 检查端口
lsof -i :1234
```

**解决：**
```bash
# 清理并重启
pkill -9 qemu-system-riscv64
./test-debug.sh
```

### 问题 3: GDB 连接卡住
**原因：** QEMU 10.x 与 `-nographic` 参数冲突

**解决：** 已在配置中移除 `-nographic`，QEMU 会打开窗口

### 问题 4: "No such file or directory"
**原因：** kernel 未构建

**解决：**
```bash
make clean
make kernel/kernel fs.img
```

---

## 🎓 调试技巧

### 设置断点
在 `kernel/main.c` 等文件中点击行号左侧设置断点

### 常用断点位置
- `kernel/main.c:main()` - 内核入口
- `kernel/proc.c:scheduler()` - 调度器
- `kernel/trap.c:usertrap()` - 系统调用入口
- `kernel/syscall.c:syscall()` - 系统调用分发

### GDB 命令
在 DEBUG CONSOLE 中输入 GDB 命令：
```gdb
-exec info registers       # 查看寄存器
-exec backtrace           # 查看调用栈
-exec x/10i $pc           # 反汇编当前位置
-exec info threads        # 查看线程（CPU）
```

### 多核调试
xv6 默认 3 个 CPU，可以：
1. 在"调用堆栈"面板切换线程（CPU）
2. 每个 CPU 独立的 PC 和寄存器

---

## 📊 测试结果

```
$ ./test-debug.sh

==========================================
xv6-riscv 调试环境测试
==========================================

步骤 1: 检查必要工具
----------------------------------------
检查 QEMU... ✓ QEMU emulator version 10.1.0
检查 GDB... ✓ GNU gdb (GDB) 16.3
检查 make... ✓

步骤 2: 检查构建产物
----------------------------------------
检查 kernel/kernel... ✓
  类型: ELF 64-bit LSB executable, UCB RISC-V
  大小: 278K
  调试信息: 包含
检查 fs.img... ✓ 2.0M

步骤 3: 清理旧进程
----------------------------------------
清理 QEMU 进程... 无需清理
清理 GDB 进程... 无需清理

步骤 4: 启动 QEMU（调试模式）
----------------------------------------
QEMU PID: 62367
检查 QEMU 进程... ✓ 运行中
检查 GDB 端口 1234... ✓ 监听中

步骤 5: 测试 GDB 连接
----------------------------------------
pc             0x1000	0x1000
=> 0x1000:	auipc	t0,0x0
   0x1004:	addi	a2,t0,40
   ...

==========================================
✓ GDB 连接成功！
==========================================
```

---

## 🔧 手动 GDB 调试

如果 VSCode 有问题，可以使用命令行 GDB：

```bash
# 终端 1: 启动 QEMU
qemu-system-riscv64 -machine virt -bios none -kernel kernel/kernel \
  -m 128M -smp 3 \
  -global virtio-mmio.force-legacy=false \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -s -S

# 终端 2: 启动 GDB
riscv64-elf-gdb kernel/kernel

# GDB 命令
(gdb) target remote localhost:1234
(gdb) break main
(gdb) continue
```

---

## 📝 下次调试

1. 直接按 **F5**
2. 或运行 `./test-debug.sh` 验证环境
3. 查看此文档解决问题

**Good luck with debugging xv6!** 🚀