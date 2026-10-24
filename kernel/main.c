#include <airi/fpu.h> // 用户浮点状态在首次用户程序以前建立。
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。

static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。

#ifndef AIRI_DEMO_MESSAGE  // 允许修改实验在编译时替换输出文本。
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 普通配置默认输出的学习标记文字。
#endif  // 结束默认演示文字的条件定义
#ifdef AIRI_BOOT_SERVICE // 专用构建配置明确选择无屏常驻路径。
static const bool diagnostics = false; // 不在开机时运行教学故障、磁盘写入或进程演示。
#else // normal和旧故障配置继续保留完整教学启动。
static const bool diagnostics = true; // 默认运行方式和旧检查点的输入行为保持。
#endif // 这里只选择启动策略，不改变用户ABI。
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 引导适配交入稳定启动描述；_Noreturn 声明内核不返回加载器。
    const char *error = airi_boot_validate(info);  // 校验必需启动字段，NULL 表示通过，字符串给出失败原因。
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    if (!airi_fpu_init()) // 诊断和常驻启动都必须拥有完整用户CPU状态。
        airi_panic("CPU requires FXSR, SSE and SSE2 user state support"); // 不把不支持目标带入用户ELF。
    airi_log("[BOOT_MODE] name=%s", diagnostics ? "diagnostic" : "service"); // 当前镜像的启动策略明确可见。
    if (diagnostics) // 服务启动只安装异常入口，不主动触发错误。
        airi_trap_demo(); // 诊断配置验证两种异常栈形状。
#ifdef AIRI_TEST_FATAL_TRAP // 只在作者故障镜像中执行未处理异常。
    airi_trap_fatal_demo(); // 必须报告并停止，不能发布 READY。
#endif // 普通镜像继续初始化。
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印协议版本和有效内存区域数，%u 与无符号参数匹配。
             info->loader);  // 输出适配层复制且有 NUL 终止的加载器名称。
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 串口分别记录内核物理位置、虚拟地址和字节长度。
             (unsigned long long)info->kernel_physical_base,  // 转换到 %llx 要求的无符号 long long。
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 虚拟地址采用十六进制，内核大小按字节十进制输出。
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 记录把物理地址换成直接映射虚拟地址的偏移量。
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只遍历启动描述声明的有效区域，不读取固定数组剩余项。
        const struct airi_memory_region *r = &info->regions[i];  // 借用第 i 条区域描述。
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 逐项输出下标、物理起点、长度及用途。
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 地址和长度都是64位范围，按 long long 适配格式。
                 airi_memory_name(r->kind));  // 将区域种类转成可读名称，不改变区域数据。
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 按实际种类累加可用区域字节数，不等同虚拟机总配置内存。
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 输出可用内存与暂由加载器占用的可回收内存两个值。
             (unsigned long long)usable,  // 可用字节数转换匹配 %llu。
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 只初始化页池、自有页表与最小堆，不依赖教学观察。
    if (diagnostics) // 用户可保留完整旧启动回归，也可选择不运行演示。
        airi_memory_demo(); // 页面、权限故障和堆容量均在资源就绪后观察。
    airi_timer_init(); // 服务与诊断都需要真实周期时钟，不能靠demo偷偷初始化。
    if (diagnostics) { // 已安装时钟后才演示hlt与主动任务切换。
        airi_timer_demo(); // 此时不再重置计数或重配PIC。
        airi_task_demo(); // 服务启动不创建这两个临时演示任务。
    }
    int block_error = airi_block_init(info); // 固定单队列设备，未连接时明确停止当前存储阶段。
    if (block_error) { // 默认运行必须带独立实验盘。
        airi_log("[BLOCK_INIT_ERROR] error=%d", block_error); // 先记录具体设备错误。
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    if (diagnostics) { // 设备初始化与恢复不需要先写教学扇区。
        airi_block_demo(); // 诊断配置保留真实DMA与FLUSH观察。
        airi_journal_demo(); // 只在诊断配置执行临时区域的提交与坏尾演示。
    }
    int storage_error = airi_storage_init(); // 启动扫描，绝不自动追加正式记录。
    if (storage_error) { // I/O错误不能当作空盘。
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_init(); // 所有设备配置都初始化固定资源池，设备错误由CQ表达。
    airi_user_init(); // 正式初始化FD资源，不借用户演示建立隐含前置。
    if (diagnostics) { // 两种模式共用已初始化的设备、队列与用户资源。
        airi_async_demo(); // 真实异步操作与错误仅在诊断配置观察。
        airi_user_demo(); // 非法用户、fork/exec和I/O完整回归保持。
    }
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = diagnostics && airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 宽高转为与 %llu 相同的参数类型。
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    if (!diagnostics) { // 常驻启动不先进入monitor，也不宣称教学阶段演示已经运行。
        airi_log("[AIRI_SERVICE_RESOURCES_READY] input=UART heap=minimal storage=recovered framebuffer=%u", airi_framebuffer_valid(&info->framebuffer)); // 这里只表示内核资源就绪，服务可用还需真实用户提示符。
#ifdef AIRI_TEST_INIT_FAILURE // 故障镜像用已有截断ELF演示装载失败后的恢复。
        const char *initial = "broken"; // 不修改正常init二进制或磁盘内容。
#else // 普通常驻镜像从同一个初始目录启动用户监督者。
        const char *initial = "init"; // 首个用户PID自然从1开始，不绕过fork/exec/wait。
#endif // 两条路径都走完整资源准备与回滚。
        int result = airi_user_boot(initial); // 正常init不返回；只有装载失败或整个根会话停止才继续。
        airi_log("[SERVICE_RECOVERY] init=%s result=%d monitor=UART", initial, result); // 退出不能假装仍在提供服务。
        airi_serial_write("airi> "); // 退回可操作的串口监控台，保留故障诊断与显式run入口。
    } else { // 默认诊断目标保留各阶段实际演示完成标记。
        /* Written AFTER all drawing, so tests can take a completed screenshot. */
        airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
                 (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
        airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
        airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
        if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
            airi_log("[AIRI_P4_READY] files=ram pipes=blocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
        if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
            airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
        airi_serial_write("airi> ");  // 输出首个串口提示符。
        airi_log("[AIRI_P6_AVAILABLE] shell=sh demo=demo input=UART"); // 只声明入口可用，完整演示由实际shell测试验证。
    }
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
            airi_arch_idle();  // PIT每10ms唤醒后重查UART/键盘，等待期间暂停CPU。
            continue;  // 跳过当前不适用项，继续检查下一项。
        }
        uint64_t before = console.total_lines;  // 记录输入前日志尾，回车后只回显本次产生的新行。
        bool changed = airi_console_key(&console, key);  // 按键改变命令或页面状态，返回是否需要刷新。
        if (!changed)  // 被忽略的按键不产生新帧。
            continue;  // 跳过当前不适用项，继续检查下一项。
        if (key == '\n' || key == '\r') {  // 回车完成命令，才把新增输出回显到 UART。
            uint64_t first = console.total_lines - console.line_count;  // 日志环只保留 line_count 行，first 是尚可读取的最早逻辑行。
            if (before < first)  // 若本次命令挤掉旧行，先把读取起点限到仍存在的区域。
                before = first;  // 不访问已经被覆盖的逻辑行。
            for (uint64_t i = before; i < console.total_lines; ++i) {  // 逐行回显本次命令产生且仍保留的内容。
                airi_serial_write(airi_console_line(&console, i));  // 逻辑行号由环接口转换为数组下标。
                airi_serial_write("\n");  // 为该行补串口行结束。
            }
            airi_serial_write("airi> ");  // 输出首个串口提示符。
        }
        if (graphics)  // 只在启动时建立了图形路径的情况下重画。
            airi_console_draw(&console);  // 把变更后的命令与页面状态写成完整画面。
        /* Published after rendering: interaction tests observe completed frames. */
        if (key == '\n' || key == '\r' || key >= AIRI_KEY_PAGE_UP)  // 命令提交与功能键在绘图后发布可观察完成记录。
            airi_log("[CONSOLE_UPDATE] view=%u commands=%u page=%u", (unsigned)console.view,  // 输出当前页面、累计命令和内存页号，观察程序据此等待具体状态。
                     console.commands, console.map_page);  // 这两个计数来自刚处理并重画的同一命令对象。
    }
}
