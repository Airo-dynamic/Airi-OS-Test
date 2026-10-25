# 内核服务、文本会话与可选显示

main 的默认目标是串口学习系统。Make 编译启动、基础库、内存/进程/文件/I/O服务、架构设备、文本会话和 serial_display；主线树不包含前端源码、字体或图形课程。合并独立 gui 分支后，显式 desktop- 配置才读取图形自己的构建片段，并加入共享 C 绘图与 guest 适配。两种目标使用不同对象目录。

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
| QEMU guest | 自有内核、UART/PS2、自有四级页表与内核堆 | 启动、权限、用户进程、文件/管道、块设备与完成结果 |
| 个人 Linux | L01–L08程序与观察工具，服务来自Linux | 完整对照源码已提供；本轮SSH255，Linux实际编译运行未验证 |
| 可选 macOS 图形参考 | Cocoa、共享 C 代码和 reference provider | 窗口、会话内文件操作、编辑与错误反馈 |

内核直接访问 UART 设备，Linux 用户程序则通过 write 系统调用请求 Linux 服务。可选图形中的 ui_backend 是同步函数指针接口，由调用方与 provider 在同一进程中执行，尚未经过自己的 syscall 或跨进程传输。当前run从初始注册表选择静态ELF，不能装载任意宿主路径；run sh进入用户shell，由它fork/exec/wait运行目录中的程序。

## 一条 mem 命令怎样返回

input.c 从 UART 读取字节，或把 PS2 扫描码与修饰键状态转成逻辑键。main 交给 airi_console_key，m、e、m 先进入有界输入数组；回车才复制出本次命令，更新历史并分派查询。启动图统计读取已验证的 BootInfo；绑定的 read_memory 回调同步复制页池与堆的数值，随后把结果复制到文本环。main 只将本次新增且仍保留的行发送回 UART，最后发布 CONSOLE_UPDATE。

文本会话拥有输入、历史和输出环，没有颜色、窗口或字体。内置命令之外的输入可以交给一个可选同步处理函数；未安装或返回 false 时，继续报告 unknown command。命令副本位于 execute 的栈，处理函数只在本次调用期间读取，不能保存地址供以后使用。上下文对象由安装者持有，必须覆盖所有可能的调用。

PIT以100Hz触发IRQ，CS3每两个tick尝试用户轮转，CS0保持不可抢占。用户I/O等待先保存条件、输出地址和完整现场，移出可运行集合；普通启动服务栈推进设备并重查等待者，写入结果后再唤醒。全部用户阻塞时执行sti/hlt等待PIT，不把忙转当阻塞；内核monitor也在K36改为sti/hlt等待周期PIT。

## 图形分支怎样读取同一会话

gui 只增加自己的显示适配、frontend、图形观察程序、资产和课程资料，不修改 main 的入口、Makefile 或文本模型。运行图形需 K11 提供的接口；G24 接入 guest 还需要 K13。图形源码提前合入较早检查点时，默认串口构建仍选择已有主线实现。

可选的 kernel/console/display.c 借用真实 BootInfo 与 console。首次有效绘图创建静态 desktop，ui_init 把主题处理函数和 desktop 上下文绑定到 console；主题保存在 desktop。之后把文本视图映射到 Terminal 或 System 页签。ui_draw 完成一帧后才返回 true，入口随后发布 READY；缺帧缓冲时不创建图形消费者，UART 仍可运行。

主机参考另有一条路径：Cocoa 事件调用 ui_key/ui_pointer，改变文档或窗口状态，再由 ui_draw 写宿主像素表面。Files 持有目录快照，Editor 持有工作副本，provider 持有会话内文件。保存成功的确认点是 provider 返回 true，随后更新 saved 与目录快照；失败保留工作文本。Run 当前返回未接入错误，Tasks 的参考数据也不是 Mac 或 guest 遥测。Cocoa 和 reference provider 都不链接进 guest。

[图形课程](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/README.md)提供逐阶段实现，[设计文档](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/docs/design.md)维护颜色、组件与工作流。

## 数据由谁保存

| 对象 | 所有者与借用者 | 寿命与限制 |
| --- | --- | --- |
| 启动栈 | entry.S 保留静态 64 KiB 范围，C 调用链使用 | 持续有效；内核任务和用户进程另有独立带保护页的栈 |
| AiriBootInfo | Limine 适配持有，内核与可选显示借用 | 地图元数据已复制；自有CR3保留所需HHDM和设备映射，像素存储仍归设备 |
| console | 内核 main 或宿主平台持有，输入处理与可选 UI 访问 | 初始化会清空扩展绑定，需要在初始化后安装 |
| desktop 和主题 | 可选图形平台持有，应用与绘图函数访问 | 后续回调不能借用已经退出的局部对象 |
| reference 文件 | 可选宿主 provider 持有 | 退出参考进程即销毁，没有持久化 |
| framebuffer | 引导环境建立映射，可选 renderer 借用 | 自有页表为有效像素范围建立设备映射，不纳入普通可借页池 |

## 课程、分支与导出

main 基础18节点之后已追加K14–K52、L02–L08，当前64节点。00–02 保持实现字节及学习连续性，03–10 继续启动和输入，K11–K13 完成串口日常入口，L01 是必修 Linux 对照。作者工具 T01–T03 沿 main 继续，不要求图形课程完成。

gui 从 main 的 00 根提交分叉，以 15 个节点 G11–G24、G26 只增加图形自己的路径，可无冲突合入任一现有 main 阶段。分支起点决定哪些路径可以独立维护，课程的 requires 则说明重建和运行需要哪些知识与实现；图形的 K11/K13 运行前置不改变分支起点。

主线清单 checkpoints.json 只保存 main；独立 graphics.json 随 gui 文件合入后补充图形节点、所属路径和终点。learn 默认处理纯 main，export 按所选主线检查点及可选图形停点闭合依赖，公共基础只应用一次。history.json 描述当前 main 检查点提交链，导出记录固定实际使用的 main/gui 来源。

HISTORY 保存各检查点的独立重建目录。图形检查点包含其需要的主线基础，指定组合也可用 export 的 --alongside 导出。已有导出目标一律拒绝覆盖；HISTORY 只允许替换同一生成器产生且未被编辑的内容。

## 无屏、WebUI 与硬件扩展

当前由内核服务持有文件、任务与 I/O 状态，文本命令、本地图形和未来 WebUI 分别读取这些服务。用户态和 IPC 完成后，桌面才可能成为可选用户程序。当前shell只独占UART字节输入，没有通用TTY/PTY、用户态窗口服务或IPC；图形monitor拒绝run sh并给出串口入口，串口继续承担故障报告。

WebUI 优先由宿主网关连接串口或虚拟通道，HTTP 在 Mac/Linux 上运行，guest 提供真实状态。设备自行提供 HTTP 还需要驱动、网络栈和用户服务。现有 C 像素代码不会直接成为浏览器界面；颜色、布局与工作流可以参考，呈现适配需另行实现。

当前只支持 x86-64 QEMU q35。CPU 入口、异常、页表和上下文属于 arch，板级初始化与设备地址属于平台/驱动。AiriBootInfo 仍要求当前的直接映射等条件。第二个真实目标确定后再验证可共享的机制，不预置没有实现的多架构入口。

## P2 的内存所有权

pages.c只把低1GiB内完整usable页纳入池，静态位图区分可管理与已借出，页零、内核和加载器占用不借出。paging.c从同一池取得四级表，memory.c映射内核、所需HHDM和有效帧缓冲后才装CR3；text只读可执行，常量只读NX，数据可写NX。内核物理别名也关闭text/常量写权限，帧缓冲采用设备缓存属性。

固定64KiB堆持有16个数据页及对应映射，小对象只借其中的payload。对象free不自动归还页面，页表解除则先断链接并失效TLB，再归还空表。当前是单核不可抢占内核，P2本身不提供用户进程；后续已实现私有用户页，仍不提供并发分配器、大页、COW或SMP失效协议。真实权限演示核对PF的RIP、错误码、CR2并只恢复精确登记的故障；普通意外异常仍停止内核。

## P3进程与P4文件的当前边界

进程有独立低半区、用户页和内核陷入栈，高半区借用长期内核表。PIT仅在CS3切换用户，CS0保持不可抢占；fork逐页复制，exec先准备完整映像再保留PID提交，wait真正阻塞并在孩子退出后唤醒。栈必须离开后才能回收，不能在exit当前调用链里释放。静态ELF与自有libc不含动态链接或信号；K41补齐用户x87/SSE2现场，内核C仍只用通用寄存器。

K24加入每进程FD表、引用计数打开对象与平坦RAM名字。fork增加引用，exec转移表并关闭CLOEXEC，exit立即关闭FD；偏移属于打开对象。同步文件传输使用有界内核副本，read先验证完整用户输出才允许推进偏移。文件内容在本次启动内保留，重启丢失，图形Files参考后端尚未接入这些真实文件。

## P4块设备与日志

VirtIO适配固定legacy PCI transport，通用块接口不暴露端口。队列和单在途请求属于连续内核BSS，设备使用guest PA；超时reset后保留并隔离静态DMA，不向新请求复用。同步等待临时开PIT IRQ但不抢占CS0。

日志正式区16–143、临时演示4–7与块演示盘末尾分开。显式little endian、sequence和双CRC构成记录格式，失效旧提交及下一空尾、同步数据、同步commit后才确认追加；错误结果可能不确定，需scan恢复。跨QEMU恢复与缓存模型各自记录，不代替主机掉电测试。管道使用独立64字节环与端点对象，FD引用数和端点存在计数分开，K28先发布非阻塞，K29再接入阻塞与条件等待。

## P5与P6的等待、完成和输入所有权

engine.c管理8个owner、32个512字节注册副本、4个全局QUEUED/ACTIVE请求和每owner四个CQ信用。接受请求即预留完成容量，buffer到CQ收取前都BUSY；cookie关联结果，代数handle和owner epoch隔离旧资源。fork新owner为空，exec同PID新epoch，exit取消QUEUED但保留ACTIVE的内核副本，真实完成后丢弃旧轮次结果。设备单队列且同时一条在途，DMA始终使用驱动静态区；超时隔离后不重用该区，不把取消逻辑等同硬件已停止访问。

UART输入只给直接启动的sh根FD0，CLOEXEC确保子程序成功替换后不再抢shell输入。用户read经过完整指针检查和有界内核副本，无数据用已有IO_READ等待；shell解析行、fork孩子、exec目标并wait。demo从用户页经RAM文件和注册副本走到独立块实验区，WRITE、FLUSH、READ分别等CQ，读回一致后再释放注册资源。用户页在退出时统一回收。

[完整shell操作](../dev-log/core/33-shell-pipeline.md)说明具体序列。[知识地图](knowledge-map.md)区分已经实现的机制、待Linux实测的路径和仍缺的能力。当前有用的可观察边界是数值状态、句柄和请求轨迹；ui_backend内部函数指针仍不能当系统调用或网络协议。

## P7 的持续资源回收

K34 起，父退出时将直接孩子交给内核托管（parent=0），不改变孩子的地址空间、FD 与等待现场。已结束孤儿与后来退出的活孤儿都进入REAP_PENDING，由另一条安全内核栈上的collect_pending回收。当前会话根的统计仍保留到launch收尾，K36另提供显式run init的常驻监督入口。后续部署扩展与第三方接入边界见[部署路线](deployment-roadmap.md)。

K35 将用户动态页限定在独立的低半区范围，按整段预检、unmap/invlpg、归还物理页、压缩owned列表的顺序释放。分配从低端寻找连续虚拟洞，owned顺序不作为布局前提；K35停点保持每次一至四页与总六十四页预算，K37再扩展容量。接口是自有页范围ABI，后续对象分配器与POSIX映射层另行接入。

K36 的 `run init` 在同一用户会话中常驻监督前台 sh。监督者没有UART输入，成功exec的直接sh孩子按固定策略取得唯一input_pid；fork的固定FD引用本身不授权读取。shell退出时撤销资格并向init交付状态，init通过IO_SLEEP的绝对tick截止阻塞后再启动新孩子。内核监控台无输入时也sti/hlt等待PIT，仍使用100Hz周期时钟。该模式保留launch诊断返回位置，尚非通用TTY、配置化服务管理或自动正式启动流程。

K37 的当前用户页预算为每进程1024页（4MiB），包含ELF、动态页和十六页初始用户栈；每次动态申请/归还最多六十四页。控制块以七个物理页地址保存按需分配的记录页，每页170项VA/PA/权限，空尾页随count缩小归还，避免exec的新旧控制块撑破16KiB内核栈。页表、记录页和内核栈另占物理内存。ELF段预留栈后最多1008页，普通ELF栈范围为[0x6fff1000,0x70001000)，其下保持未映射；没有自动扩栈、COW或通用malloc。K37停点的内核对象堆仍是64KiB，K38再引入扩展。

K38 的内核对象堆以16页起步，空间不足时通过静态区域句柄映射下一批16页，完整成功后才接入块链，总上限256页（1MiB）。free保留backing缓存；快照另外报告heap_used，任务与用户会话按空闲页加堆数据页、活payload进行净资源核对。此规则依赖当前1MiB范围始终使用初始同一张叶子表。启动教学演示会实际增长到上限并归还全部对象，不代表自动收缩或并发分配已经实现。

K39 将初始化与诊断演示分开：所有目标都安装异常、建立内存/时钟、初始化块设备、扫描日志并建立队列与用户文件；normal继续主动故障、堆容量、磁盘写入和用户演示，service仅初始化并自动进入同一用户init。service初始堆64KiB，不先运行大对象演示；缺图形描述配置保持UART监督。资源就绪标记不代表用户服务已经可用，真实shell提示符与命令另行验证。初始ELF失败回UART恢复monitor；已运行init若退出仍按现有会话规则等待活后代。

K40在平台层为固定q35根槽PCI INTx配置PIRQ到IRQ11、电平触发和PIC级联，IDT43先于用户故障分派处理。IRQ读清设备ISR并确认从片/主片，只记录提示；完成身份、长度与数据仍由普通poll读取used验证。打断有异步工作的CS3时暂回普通服务栈，CS0继续不可抢占。PIT仍提供截止和后备，屏蔽IRQ11的测试保持真实DMA/used收集。此路由只支持当前单设备QEMU模型，不是通用共享IRQ或真实板ACPI实现。

K41为每进程单独持有一页x87/SSE2状态，用户系统调用、PIT与设备IRQ先保存，统一返回边界恢复。fork复制512字节，exec成功重新初始化、失败保持，退出归还；不映射为USER。用户编译允许SSE2，内核C保持general-only，专用初始化汇编清空旧数据。未开放AVX/OSXSAVE，K42再提供用户FS基址，K43补齐静态local-exec模板，尚无多线程或动态TLS。

K42将FS与浮点保存集中在共同CS3入口，读取真实FS后清零供内核处理；所有用户返回包括无新设备ISR的分支都恢复。调用33/34设置/读取低半区基址，设置不固定页面，访问仍受用户页权限检查。fork继承、exec成功归零、失败保持；只承诺基址接口，不保留任意用户段选择子或开放FSGSBASE。

K43解析一个受限PT_TLS，校验文件/VA与LOAD对应、4KiB模板上限和对齐，额外运行页计入用户预算。prepare_elf在固定0x60000000创建RW/NX副本，TP前放静态TLS、TP后放self与保留字；FS在main前准备。无TLS程序仍FS0，fork复制修改值，exec重新复制初值；没有DTV、动态模块或C++TLS构造/析构。

K44在用户posix.o中用_Thread_local保存errno，将负错误转为-1和正错误号；read/write/pread/pwrite一次最多提交256字节，保留实际短返回。标准名字及自有头不进入内核或宿主检查。仅需要者显式链接，原始ABI与无TLS程序保持；open标志转换、有界绝对定位、dup/pipe/getpid/_exit属于当前子集，该检查点尚不提供完整权限、相对定位、信号或stdio；K46再接有符号定位。

K45的35/36号原始调用提供preadv/pwritev，一次复制最多八项固定16字节描述，总量先检查有符号溢出，实际只处理前256字节。全部实际前缀验证后再操作文件一次，读回仅分散真实数量；零长不访问基址，流ESPIPE。用户POSIX层只转换参数和errno，不解引用描述；尚无阻塞readv/writev或异步任意向量。

K46通过37号调用在文件层一次取SET/CUR/END基准并验证差值，所有失败先不改offset。最终范围仍限0至1024，定位不扩size；INT64_MIN用安全幅度运算拒绝。用户lseek改用此入口，旧14号绝对接口保留。当前单核不可抢占，未来SMP仍需打开对象同步。

K47的38号调用从同一PIT快照返回16字节秒/纳秒，用户clock_gettime仅支持MONOTONIC1。名义100Hz已交付tick量化到10ms，长关中断可能漏计，非准确墙钟或高精度clocksource；REALTIME明确拒绝，完整用户输出由内核验证。

K48将纯heap.c以用户选项再编译成独立对象，仅malloc-probe选择对象包装与posix.o。每进程单线程、按需256KiB arena；16字节请求头记录真实长度，free只归还对象，页面随exit/exec回收。calloc先查乘法，realloc增长先申请后复制再释放，失败保留旧对象；不提供并发、自动扩缩与完整stdlib。新增后的237字节完整程序目录仍使用原256字节容量。

K49将页表根、记录页和数量收进96字节user_memory，页管理辅助函数只接该所有者。exec以take_memory转移并清来源，保留PID/父关系/已运行tick/内核栈/FD/异步owner，仅替换用户内存、FPU和FS，切CR3后destroy_memory旧内存。当前仍唯一内嵌持有，不等于共享线程资源或引用计数。

K50将FS/FPU、内核陷入栈、frame/initial、时钟/故障与等待条件收进424字节user_execution，user_process保留进程资源并暂内嵌一个执行者，整体800字节。create_stack显式接执行者和合法槽号，create_fpu/place_frame只接执行者；destroy_execution必须在其他栈上调用。调度和陷入ABI保持，尚未提供共享线程。

K51修正TLS关联校验：初值filesz必须完整对应某个LOAD文件范围，零尾部memsz不要求被普通LOAD memsz包住；模板起点仍锚定合法段，纯零模板可在段右端。运行副本页预算与清零保持，ctls-tail实际TLS4/273、普通LOAD4/5，分别读写互不覆盖。

K52保留八进程组，增加全局八worker，调度遍历十六execution。current指所属进程，running指实际执行者；process唯一拥有共享内存/FD/异步owner，execution独立持有FS、FPU、内核栈与等待。创建继承父FPU环境并从ELF重新建立TLS；主执行者负责创建/join，工作return保留结果，原始exit或用户故障结束整组。join后安全归还内核栈/FPU，用户栈/TLS最多144页缓存至进程exec/exit。并发期间拒绝fork/exec/waitpid/release，等待中的FD拒绝close；无pthread/detach/cancel、主执行者独立退出或线程安全malloc。目录实测255字节，仍用256容量。

K53以进程对象和用户VA作为私有futex键，43号比较四字节条件后登记execution等待，44号最多通知十六人。原子性依据固定单CPU和不可抢占内核，不能泛化到SMP。完成出口统一交付原frame结果；poll只处理到期，wake不改字也不授锁，整组退出撤销等待资格后安全回收。新增程序使目录含NUL达261字节，内核/用户查询容量512，shell按原write上限分段并处理短返回。

K54增加可由C/C++头包含的四字节私有互斥：强CAS以TID记录所有权，acquire/release交接普通数据，失败用K53等待并按唯一PIT截止重试。非持有者不能解锁，自重入-35；非robust持有者返回后字不自动清理。每次调用仍查询内核TID，unlock总通知一人，不记为零系统调用快速路径。K48分配器仍单线程。
