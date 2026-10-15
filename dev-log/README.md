# AiriOS · 10

This is a reconstructed teaching checkpoint. Follow the available chapters in order; later kernel mechanisms are not implemented here. GUI is maintained separately and has explicit prerequisites.

- [00 · 从空目录找到并运行一个程序](00-first-tool.md)
- [01 · 让构建使用正确的一份工具](01-tools.md)
- [02 · 让外部引导材料有可验证的身份](02-dependencies.md)
- [03 · 让字节复制在重叠时仍然正确](core/03-bytes.md)
- [04 · 在容量有限的数组里形成一行日志](core/04-format.md)
- [05 · 把启动信息变成可检查的数据](core/05-boot-contract.md)
- [06 · 让引导器进入自己的 C 代码，并发出第一行文字](core/06-entry.md)
- [07 · 把 ELF 放入 UEFI 镜像并看到第一行](core/07-first-boot.md)
- [08 · 将真实引导响应复制到自己的接口](core/08-real-boot-info.md)
- [09 · 先让普通字符成为一条可执行命令](core/09-console.md)
- [10 · 把串口和键盘字节接入内核命令模型](core/10-input.md)
