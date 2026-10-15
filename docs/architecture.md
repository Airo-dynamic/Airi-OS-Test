# 内核服务、文本会话与可选显示

main 的默认目标是串口学习系统。Make 只编译启动、基础库、内存管理、架构设备、文本会话和 serial_display；主线树不包含前端源码、字体或图形课程。合并独立 gui 分支后，显式 desktop- 配置才读取图形自己的构建片段，并加入共享 C 绘图与 guest 适配。两种目标使用不同对象目录。

```text
OVMF → Limine → entry.S → Limine 适配 → AiriBootInfo → kernel_main
                                                        ↓
                             UART / PS2 → 文本会话 → 串口命令结果
                                                        ↓
                       显示接口 → 默认实现：返回未绘图
                                → 可选图形实现：desktop → framebuffer
```

入口、基础库、启动描述与文本命令属于 main。两份显示实现使用相同的 airi_console_draw 名字，构建只链接选中的一份。返回 false 表示本次没有绘图；BootInfo 仍可能包含未使用的有效 framebuffer 描述。主循环负责 UART 发送，serial_display 本身不发送文字。

## 调用发生在哪个环境

| 环境 | 执行内容与服务来源 | 当前可观察的行为 |
| --- | --- | --- |
| macOS/Linux 构建主机 | Python、Make、LLVM、QEMU，由主机操作系统提供服务 | 工具选择、独立 ELF、镜像和主机检查 |
| QEMU guest | 自有内核、UART/PS2、自有四级页表与内核堆 | 实际启动、内存地图、设备输入与命令结果 |
| 个人 Linux | ordinary/raw 程序和 GDB，服务来自 Linux | 进程入口、调用约定与系统调用；实测情况见验证记录 |
| 可选 macOS 图形参考 | Cocoa、共享 C 代码和 reference provider | 窗口、会话内文件操作、编辑与错误反馈 |

内核直接访问 UART 设备，Linux 用户程序则通过 write 系统调用请求 Linux 服务。可选图形中的 ui_backend 是同步函数指针接口，由调用方与 provider 在同一进程中执行，尚未经过自己的 syscall 或跨进程传输。当前串口命令也不能启动任意 ELF。

## 一条 mem 命令怎样返回

input.c 从 UART 读取字节，或把 PS2 扫描码与修饰键状态转成逻辑键。main 交给 airi_console_key，m、e、m 先进入有界输入数组；回车才复制出本次命令，更新历史并分派查询。启动图统计读取已验证的 BootInfo；绑定的 read_memory 回调同步复制页池与堆的数值，随后把结果复制到文本环。main 只将本次新增且仍保留的行发送回 UART，最后发布 CONSOLE_UPDATE。

文本会话拥有输入、历史和输出环，没有颜色、窗口或字体。内置命令之外的输入可以交给一个可选同步处理函数；未安装或返回 false 时，继续报告 unknown command。命令副本位于 execute 的栈，处理函数只在本次调用期间读取，不能保存地址供以后使用。上下文对象由安装者持有，必须覆盖所有可能的调用。

当前尚无任务调度。空闲循环调用 airi_arch_relax，x86 实现使用 pause 提示忙等。异常入口已经保存CPU现场；设备时钟、任务等待与唤醒继续在P3/P5实现。

## 图形分支怎样读取同一会话

gui 只增加自己的显示适配、frontend、图形观察程序、资产和课程资料，不修改 main 的入口、Makefile 或文本模型。运行图形需 K11 提供的接口；G24 接入 guest 还需要 K13。图形源码提前合入较早检查点时，默认串口构建仍选择已有主线实现。

可选的 kernel/console/display.c 借用真实 BootInfo 与 console。首次有效绘图创建静态 desktop，ui_init 把主题处理函数和 desktop 上下文绑定到 console；主题保存在 desktop。之后把文本视图映射到 Terminal 或 System 页签。ui_draw 完成一帧后才返回 true，入口随后发布 READY；缺帧缓冲时不创建图形消费者，UART 仍可运行。

主机参考另有一条路径：Cocoa 事件调用 ui_key/ui_pointer，改变文档或窗口状态，再由 ui_draw 写宿主像素表面。Files 持有目录快照，Editor 持有工作副本，provider 持有会话内文件。保存成功的确认点是 provider 返回 true，随后更新 saved 与目录快照；失败保留工作文本。Run 当前返回未接入错误，Tasks 的参考数据也不是 Mac 或 guest 遥测。Cocoa 和 reference provider 都不链接进 guest。

[图形课程](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/README.md)提供逐阶段实现，[设计文档](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/docs/design.md)维护颜色、组件与工作流。

## 数据由谁保存

| 对象 | 所有者与借用者 | 寿命与限制 |
| --- | --- | --- |
| 启动栈 | entry.S 保留静态 64 KiB 范围，C 调用链使用 | 当前持续有效，后续任务尚无各自的栈 |
| AiriBootInfo | Limine 适配持有，内核与可选显示借用 | 地图元数据已复制；像素地址仍指向引导映射 |
| console | 内核 main 或宿主平台持有，输入处理与可选 UI 访问 | 初始化会清空扩展绑定，需要在初始化后安装 |
| desktop 和主题 | 可选图形平台持有，应用与绘图函数访问 | 后续回调不能借用已经退出的局部对象 |
| reference 文件 | 可选宿主 provider 持有 | 退出参考进程即销毁，没有持久化 |
| framebuffer | 引导环境建立映射，可选 renderer 借用 | 回收启动范围或重建页表时须处理这块映射的寿命 |

## 课程、分支与导出

main 基础18节点之后已追加K14–K24、L02–L03，当前31节点。00–02 保持实现字节及学习连续性，03–10 继续启动和输入，K11–K13 完成串口日常入口，L01 是必修 Linux 对照。作者工具 T01–T03 沿 main 继续，不要求图形课程完成。

gui 从 main 的 00 根提交分叉，以 15 个节点 G11–G24、G26 只增加图形自己的路径，可无冲突合入任一现有 main 阶段。分支起点决定哪些路径可以独立维护，课程的 requires 则说明重建和运行需要哪些知识与实现；图形的 K11/K13 运行前置不改变分支起点。

主线清单 checkpoints.json 只保存 main；独立 graphics.json 随 gui 文件合入后补充图形节点、所属路径和终点。learn 默认处理纯 main，export 按所选主线检查点及可选图形停点闭合依赖，公共基础只应用一次。history.json 描述当前 main 检查点提交链，导出记录固定实际使用的 main/gui 来源。

HISTORY 保存各检查点的独立重建目录。图形检查点包含其需要的主线基础，指定组合也可用 export 的 --alongside 导出。已有导出目标一律拒绝覆盖；HISTORY 只允许替换同一生成器产生且未被编辑的内容。

## 无屏、WebUI 与硬件扩展

后续由内核服务持有文件、任务与 I/O 状态，文本命令、本地图形和未来 WebUI 分别读取这些服务。用户态和 IPC 完成后，桌面才可能成为可选用户程序。当前尚未实现 TTY/PTY、用户态窗口服务或 IPC，串口继续承担早期故障报告。

WebUI 优先由宿主网关连接串口或虚拟通道，HTTP 在 Mac/Linux 上运行，guest 提供真实状态。设备自行提供 HTTP 还需要驱动、网络栈和用户服务。现有 C 像素代码不会直接成为浏览器界面；颜色、布局与工作流可以参考，呈现适配需另行实现。

当前只支持 x86-64 QEMU q35。CPU 入口、异常、页表和上下文属于 arch，板级初始化与设备地址属于平台/驱动。AiriBootInfo 仍要求当前的直接映射等条件。第二个真实目标确定后再验证可共享的机制，不预置没有实现的多架构入口。

## P2 的内存所有权

pages.c只把低1GiB内完整usable页纳入池，静态位图区分可管理与已借出，页零、内核和加载器占用不借出。paging.c从同一池取得四级表，memory.c映射内核、所需HHDM和有效帧缓冲后才装CR3；text只读可执行，常量只读NX，数据可写NX。内核物理别名也关闭text/常量写权限，帧缓冲采用设备缓存属性。

固定64KiB堆持有16个数据页及对应映射，小对象只借其中的payload。对象free不自动归还页面，页表解除则先断链接并失效TLB，再归还空表。当前是单核不可抢占内核，P2本身不提供用户进程；后续已实现私有用户页，仍不提供并发分配器、大页、COW或SMP失效协议。真实权限演示核对PF的RIP、错误码、CR2并只恢复精确登记的故障；普通意外异常仍停止内核。

## P3进程与P4文件的当前边界

进程有独立低半区、用户页和内核陷入栈，高半区借用长期内核表。PIT仅在CS3切换用户，CS0保持不可抢占；fork逐页复制，exec先准备完整映像再保留PID提交，wait真正阻塞并在孩子退出后唤醒。栈必须离开后才能回收，不能在exit当前调用链里释放。静态ELF与自有整数libc不含动态链接、信号或FPU/SIMD现场。

K24加入每进程FD表、引用计数打开对象与平坦RAM名字。fork增加引用，exec转移表并关闭CLOEXEC，exit立即关闭FD；偏移属于打开对象。同步文件传输使用有界内核副本，read先验证完整用户输出才允许推进偏移。文件内容在本次启动内保留，重启丢失，图形Files参考后端尚未接入这些真实文件。
