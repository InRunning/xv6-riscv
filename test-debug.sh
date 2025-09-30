#!/bin/bash

# xv6-riscv 调试环境测试脚本
# 用于逐步验证 QEMU + GDB 调试环境

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "xv6-riscv 调试环境测试"
echo "=========================================="
echo ""

# 步骤 1: 检查工具
echo -e "${YELLOW}步骤 1: 检查必要工具${NC}"
echo "----------------------------------------"

echo -n "检查 QEMU... "
if command -v /opt/homebrew/bin/qemu-system-riscv64 &> /dev/null; then
    QEMU_VERSION=$(/opt/homebrew/bin/qemu-system-riscv64 --version | head -1)
    echo -e "${GREEN}✓${NC} $QEMU_VERSION"
else
    echo -e "${RED}✗ 未找到 qemu-system-riscv64${NC}"
    exit 1
fi

echo -n "检查 GDB... "
if command -v /opt/homebrew/bin/riscv64-elf-gdb &> /dev/null; then
    GDB_VERSION=$(/opt/homebrew/bin/riscv64-elf-gdb --version | head -1)
    echo -e "${GREEN}✓${NC} $GDB_VERSION"
else
    echo -e "${RED}✗ 未找到 riscv64-elf-gdb${NC}"
    exit 1
fi

echo -n "检查 make... "
if command -v make &> /dev/null; then
    echo -e "${GREEN}✓${NC}"
else
    echo -e "${RED}✗ 未找到 make${NC}"
    exit 1
fi

echo ""

# 步骤 2: 检查构建产物
echo -e "${YELLOW}步骤 2: 检查构建产物${NC}"
echo "----------------------------------------"

echo -n "检查 kernel/kernel... "
if [ -f "kernel/kernel" ]; then
    KERNEL_INFO=$(file kernel/kernel)
    echo -e "${GREEN}✓${NC}"
    echo "  类型: $KERNEL_INFO"
    echo "  大小: $(ls -lh kernel/kernel | awk '{print $5}')"

    # 检查是否包含调试信息
    if echo "$KERNEL_INFO" | grep -q "with debug_info"; then
        echo -e "  调试信息: ${GREEN}包含${NC}"
    else
        echo -e "  调试信息: ${RED}缺失${NC}"
    fi
else
    echo -e "${RED}✗ 文件不存在，需要先构建${NC}"
    echo "运行: make kernel/kernel"
    exit 1
fi

echo -n "检查 fs.img... "
if [ -f "fs.img" ]; then
    echo -e "${GREEN}✓${NC} $(ls -lh fs.img | awk '{print $5}')"
else
    echo -e "${YELLOW}⚠${NC} 文件不存在，将自动构建"
fi

echo ""

# 步骤 3: 清理旧进程
echo -e "${YELLOW}步骤 3: 清理旧进程${NC}"
echo "----------------------------------------"

echo -n "清理 QEMU 进程... "
pkill -9 qemu-system-riscv64 2>/dev/null && echo -e "${GREEN}已清理${NC}" || echo "无需清理"

echo -n "清理 GDB 进程... "
pkill -9 riscv64-elf-gdb 2>/dev/null && echo -e "${GREEN}已清理${NC}" || echo "无需清理"

sleep 1
echo ""

# 步骤 4: 启动 QEMU
echo -e "${YELLOW}步骤 4: 启动 QEMU（调试模式）${NC}"
echo "----------------------------------------"

echo "启动命令："
echo "/opt/homebrew/bin/qemu-system-riscv64 \\"
echo "  -machine virt \\"
echo "  -bios none \\"
echo "  -kernel kernel/kernel \\"
echo "  -m 128M \\"
echo "  -smp 3 \\"
echo "  -global virtio-mmio.force-legacy=false \\"
echo "  -drive file=fs.img,if=none,format=raw,id=x0 \\"
echo "  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \\"
echo "  -s -S"
echo ""

# 启动 QEMU
/opt/homebrew/bin/qemu-system-riscv64 \
  -machine virt \
  -bios none \
  -kernel kernel/kernel \
  -m 128M \
  -smp 3 \
  -global virtio-mmio.force-legacy=false \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -s -S > /tmp/qemu-xv6.log 2>&1 &

QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"
echo "日志文件: /tmp/qemu-xv6.log"

echo -n "等待 QEMU 启动..."
sleep 3
echo -e " ${GREEN}完成${NC}"

# 检查进程
echo -n "检查 QEMU 进程... "
if ps -p $QEMU_PID > /dev/null 2>&1; then
    echo -e "${GREEN}✓ 运行中${NC}"
else
    echo -e "${RED}✗ 进程已退出${NC}"
    echo "查看日志:"
    cat /tmp/qemu-xv6.log
    exit 1
fi

# 检查端口
echo -n "检查 GDB 端口 1234... "
if lsof -i :1234 > /dev/null 2>&1; then
    echo -e "${GREEN}✓ 监听中${NC}"
    lsof -i :1234 | tail -1
else
    echo -e "${RED}✗ 未监听${NC}"
    ps aux | grep qemu | grep -v grep
    exit 1
fi

echo ""

# 步骤 5: 测试 GDB 连接
echo -e "${YELLOW}步骤 5: 测试 GDB 连接${NC}"
echo "----------------------------------------"

# 创建 GDB 测试脚本
cat > /tmp/test-gdb.cmd << 'EOF'
set pagination off
set confirm off
target remote localhost:1234
info registers pc
print $pc
x/5i $pc
backtrace
quit
EOF

echo "GDB 测试命令："
cat /tmp/test-gdb.cmd
echo ""

echo "执行 GDB 连接测试..."
echo "----------------------------------------"

/opt/homebrew/bin/riscv64-elf-gdb -nx -batch -x /tmp/test-gdb.cmd kernel/kernel 2>&1 | tee /tmp/gdb-test-output.txt

if [ ${PIPESTATUS[0]} -eq 0 ]; then
    echo ""
    echo -e "${GREEN}=========================================="
    echo "✓ GDB 连接成功！"
    echo "==========================================${NC}"
    echo ""
    echo "测试结果："
    echo "  - QEMU 正常启动并监听端口 1234"
    echo "  - GDB 成功连接并读取寄存器"
    echo "  - 调试环境配置正确"
    echo ""
    echo "现在可以在 VSCode 中按 F5 开始调试了！"
    echo ""
    echo -e "${YELLOW}提示: QEMU 仍在后台运行 (PID: $QEMU_PID)${NC}"
    echo "停止 QEMU: kill $QEMU_PID"
    echo "或运行: pkill -9 qemu-system-riscv64"
else
    echo ""
    echo -e "${RED}=========================================="
    echo "✗ GDB 连接失败"
    echo "==========================================${NC}"
    echo ""
    echo "请检查错误信息，并尝试："
    echo "1. 重新运行此脚本"
    echo "2. 查看 QEMU 日志: cat /tmp/qemu-xv6.log"
    echo "3. 查看 GDB 输出: cat /tmp/gdb-test-output.txt"

    # 清理
    kill $QEMU_PID 2>/dev/null
    exit 1
fi

echo ""
echo "按 Enter 停止 QEMU 并退出..."
read

# 清理
echo "清理进程..."
kill $QEMU_PID 2>/dev/null
pkill -9 qemu-system-riscv64 2>/dev/null

echo -e "${GREEN}完成${NC}"