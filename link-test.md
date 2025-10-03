# VSCode 文件链接测试

## 1. Comment Anchors 扩展链接

这些链接使用 Comment Anchors 扩展的 LINK 标签：

```c
// LINK user/sh.c:160
// LINK kernel/sysfile.c:435
// LINK kernel/file.c:135
```

## 2. 标准 Markdown 链接

这些是标准的 Markdown 链接：

- [user/sh.c:160](user/sh.c:160)
- [kernel/sysfile.c:435](kernel/sysfile.c:435)
- [kernel/file.c:135](kernel/file.c:135)

## 3. VSCode 内置文件链接

VSCode 内置支持以下格式的文件链接：

- 直接路径：user/sh.c#L160
- 直接路径：kernel/sysfile.c#L435
- 直接路径：kernel/file.c#L135

## 4. 绝对路径链接

- [/Users/Zhuanz/Projects/xv6-riscv/user/sh.c#L160](file:///Users/Zhuanz/Projects/xv6-riscv/user/sh.c#L160)
- [/Users/Zhuanz/Projects/xv6-riscv/kernel/sysfile.c#L435](file:///Users/Zhuanz/Projects/xv6-riscv/kernel/sysfile.c#L435)
- [/Users/Zhuanz/Projects/xv6-riscv/kernel/file.c#L135](file:///Users/Zhuanz/Projects/xv6-riscv/kernel/file.c#L135)

## 5. 测试说明

1. Comment Anchors 扩展的链接应该显示为蓝色高亮，并在上方显示可点击的 CodeLens
2. 标准 Markdown 链接应该可以正常点击
3. VSCode 内置文件链接应该可以按住 Ctrl/Cmd 并点击跳转
4. 如果链接不工作，可能需要重新加载 VSCode 窗口

## 6. 故障排除

如果链接不工作，请尝试：

1. 重新加载 VSCode 窗口（Cmd+Shift+P -> "Developer: Reload Window"）
2. 检查 Comment Anchors 扩展是否已启用
3. 确认文件路径和行号是否正确
4. 检查 VSCode 设置中的链接相关配置