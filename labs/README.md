# 从字节与启动信息开始观察状态

主线的两个宿主程序使用主机libc打印，直接调用我们写的C模块。它们没有进入guest：

- [重叠复制与格式化](host/base/memmove_trace.c)：沿第04节的同一个ABCDE数组观察正确反向复制、错误正向复制，以及容量5时的终止符与返回长度。
- [启动合同](host/boot/boot_info_probe.c)：沿第05节的两段物理地址范围，观察合法输入、版本错误、范围重叠与空段。

完成T03后，`./airios learn lab memmove`和`./airios learn lab boot-info`会构建观察程序并把输出保存在artifacts/labs。在此前的章节中，使用各节已经提供的编译与运行命令。

[Linux入口对照](linux/p1/README.md)承接K13，实际连接状态与限制见项目验证记录。可选的[GUI编辑事务观察](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/17-editing.md)在G17提供；合入并完成对应支线后才使用`./airios learn lab editor`。宿主编辑与保存不能证明guest已经具有文件系统。
