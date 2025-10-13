        # qemu -kernel 将内核加载到 0x80000000 地址
        # 并导致每个 hart (即 CPU) 跳转到该地址
        # kernel.ld 链接器将以下代码放置在 0x80000000 地址
.section .text                    # 声明当前节为 .text 节，用于存放可执行代码
.global _entry                    # 声明 _entry 为全局符号，使链接器能够找到它
_entry:                          # 内核入口点标签
        # 为 C 代码设置栈
        # stack0 在 start.c 中声明，
        # 每个CPU分配 4096 字节的栈空间，即4KB,有点小
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0          # la: Load Address - 将 stack0 的地址加载到 sp 寄存器
        li a0, 1024*4          # li: Load Immediate - 将 4096 (1024*4) 加载到 a0 寄存器
        csrr a1, mhartid       # csrr: Control and Status Register Read - 读取当前 hart (CPU) 的 ID 到 a1 寄存器
        addi a1, a1, 1         # addi: Add Immediate - 将 hartid 加 1 (hartid 从 0 开始，栈从 1 开始)
        mul a0, a0, a1         # mul: Multiply - 计算 (hartid + 1) * 4096，结果存入 a0
        add sp, sp, a0         # add: Add - 将计算出的偏移量加到 sp 上，设置每个 CPU 的栈顶
        # 跳转到 start.c 中的 start() 函数
        # //LINK:kernel/start.c
        call start             # call: 调用 start 函数，保存返回地址到 ra 寄存器
spin:                           # 死循环标签
        j spin                 # j: Jump - 无条件跳转到 spin 标签，形成死循环
