# K43 · 让 C 的本地变量从 ELF 模板建立

从 K42 继续。用户现在能手工分配一个块，将地址交给 FS，再用固定偏移读写。不过普通运行库需要直接写 `_Thread_local int value`，由编译器决定偏移，运行前自动建立每个执行者的初始副本。本节让静态 ELF 的这一条路径实际运行：`sh → ctls → fork → exec ctls-reset → wait → ctls-char → ctls-page`。

这里的 TLS 指 Thread-Local Storage，即线程本地存储，与网络连接的 TLS 加密协议是两个不同概念。当前仍每进程一个线程，所以每个进程先拥有一份；后续共享地址空间的线程还需各自分配本地块。只支持静态 local-exec 模型、最多 4 KiB 模板，不实现动态模块、__tls_get_addr 或 C++ 本地对象构造/析构。

<a id="ctls-template"></a>
## 源文件中的一个变量，文件里保存的是模板

C11 的 `_Thread_local` 改变静态存储对象的归属：同一变量名将按当前执行者取自己的实例。有初值的变量进入 ELF 的 .tdata，未显式初始化的进入 .tbss。后者像普通 BSS 一样只声明零填充长度，不要求文件里真的放下所有零。

编译器还要把变量访问变成相对线程指针 TP 的寻址，而不能把模板的绝对地址当作实例。在本轮对象文件里可看到 R_X86_64_TPOFF32 重定位，表示等待静态链接器决定相对 TP 的偏移。LLD 在最终链接时解析它，内核不在运行中再重定位。这些模板、运行副本与 x86-64 寻址模型可对照 [Drepper 的 ELF TLS 说明](https://www.akkadia.org/drepper/tls.pdf)。

ELF 的程序头 PT_TLS 给出模板文件位置、初值长度、总长度与对齐。LOAD 仍承载其普通映像范围，所以本实现先照常装载 LOAD，再另外创建真正的 TLS 运行副本。模板 VA 不成为 FS 基址，修改运行副本也不回写内核只读映像；下一次成功 exec 仍从原文件初值重新建立。

本轮 ctls 的真实布局为：.tdata 初值 72 字节，连同 .tbss 与中间填充总共 153 字节，要求 64 字节对齐。initialized 在模板 offset0，aligned 在 offset64，zero 的73个字节从 offset80开始。LOAD 的数据页与实际 TLS 页都消耗用户页预算，不能把同一份数据的两种用途混为一次分配。

<a id="ctls-layout"></a>
## 负偏移要求数据在 TP 前，还要保留较小对齐

x86-64 的静态 local-exec 在 TP 前安排变量。先看后面的 ctls-char：整个模板只有一个字节，对齐要求也是1，因此链接后的 only 位于 TP−1。运行块从0x60000000开始，但 TP 还要满足运行库至少16字节对齐的约定，放在0x60000010。要让 TP−1 读到初值，这个字节就必须复制到0x6000000f，不能仍放在块首。

TP 之后的16字节是本节最小线程控制块 TCB：第一项 uint64_t 为 self，即 TP 自身，第二项暂留零。当前 Clang 会先从 FS:0 读 self，再用已经链接好的负偏移访问变量。因此 FS.base 要指向 TP，TP 处还必须保存同一个地址；仅把 FS 设为模板地址不能建立这条寻址。

运行块固定从用户 VA 0x60000000 开始，避开 ELF、动态堆与用户栈。设模板总长为 M、对齐为 A，A=0先按1处理。链接器要求的模板跨度和运行块为 TCB 留出的跨度需要分别计算：

- `span = align_up(M, A)`：链接器据此确定模板相对 TP 的负偏移。
- `pointer = align_up(M, max(A,16))`：块起点到 TP 的偏移，让 TCB 自身至少16字节对齐。
- `data_offset = pointer - span`：初值在运行块中的实际落点。
- `pages = ceil((pointer + 16) / 4096)`：包含 TCB 的完整页面数量。

这里 align_up 对 2 的幂使用 `(value + align - 1) & ~(align - 1)`。所有输入先限制到4KiB以内，才进行向上舍入。刚才的一字节模板得到 span=1、pointer=16，二者之差15正是初值的落点。ctls 的153字节模板按64对齐后，两种跨度都为192，数据才恰好从块首开始；这个结果不能推广到较小对齐的模板。

| 实际模板 | TP偏移 | 数据偏移 | 页面及真实观察 |
| --- | --- | --- | --- |
| M=153、A=64 | 192，即0xc0 | 0 | 1页；initialized 在 TP−192，aligned 在 TP−128，zero 在 TP−112 |
| M=1、A=1 | 16 | 15 | 1页；唯一字节必须在 TP−1，而非块首 |
| M=4096、A=16 | 4096 | 0 | 2页；第二页起点存TCB，不能只映射模板一页 |

本实现只接受模板 VA 和文件位置对 A 都无额外偏斜的布局，A 最多4096。这覆盖当前固定链接脚本及观察程序；其他工具链产生的复杂静态布局需要扩展校验与布局规则。

先更新 elf.h，加入独立 TLS 事实和已计算的运行偏移。pages 继续表示 LOAD 页数，tls.pages 另记运行副本，二者都要计入栈以外的总预算。

<!-- write: include/airi/elf.h -->
```c
#ifndef AIRI_ELF_H // 静态ELF解析边界只声明一次。
#define AIRI_ELF_H // 包含保护。
#include <stdbool.h> // 校验器使用布尔范围与入口状态。
#include <airi/base.h> // 固定宽度字段与自有字节复制。
#define AIRI_ELF_SEGMENTS 8u // 教学程序最多八个LOAD段。
#include <airi/user_limits.h> // ELF验证必须为实际初始栈预留容量。
#define AIRI_ELF_PAGES (AIRI_USER_PAGES - AIRI_USER_STACK_PAGES) // 段最多1008页，栈的16页单独建立。
struct airi_elf_segment { // 从外部字节校验后复制出的段事实。
    uint64_t address, offset, file_bytes, memory_bytes; // 用户VA、映像偏移、初始化长度和总内存长度。
    unsigned flags; // 原ELF PF_R/PF_W/PF_X，不能当作x86 PTE直接写入。
};
#define AIRI_ELF_TLS_LIMIT 4096u // 本轮单静态模块最多4KiB模板，运行块含TCB另算页。
struct airi_elf_tls { // PT_TLS描述模板，偏移与实际运行块布局分开。
    uint64_t address, offset, file_bytes, memory_bytes, alignment; // 模板VA、文件位置、初值/总长度与对齐。
    unsigned pointer_offset, data_offset, pages; // 运行块中TP位置、初值落点和含16字节TCB的页数。
};
struct airi_elf_view { // 只借用输入映像，装载完成前映像必须持续有效。
    const uint8_t *image; // 不拥有也不修改这些输入字节。
    size_t bytes; // 输入映像的实际可读取长度。
    uint64_t entry; // 入口必须位于可执行段的文件内容中。
    unsigned count, pages; // 已验证LOAD段数与向外舍入后的总页数。
    struct airi_elf_tls tls; // memory_bytes为0表示无需初始化FS；不改变无TLS程序的入口。
    struct airi_elf_segment segments[AIRI_ELF_SEGMENTS]; // 每项已通过范围、权限和重叠检查。
};
const char *airi_elf_inspect(const void *image, size_t bytes, struct airi_elf_view *view); // NULL表示通过；失败不修改输出view。
#endif // 结束包含保护。
```

<a id="ctls-inspect"></a>
## 拒绝坏模板后才分配，允许程序头先出现 TLS

elf.c 不再一概拒绝 PT_TLS，但仍拒绝动态链接和解释器。TLS 校验要求：最多一个头、模板权限为只读、filesz 不大于 memsz、文件跨度完整、模板大小与对齐在本节范围。重复的空头也拒绝，不能静默让后一个覆盖前一个。

无 TLS 的普通程序在新链接脚本下也会有一项全零空描述：memsz/filesz/地址/偏移为0，对齐为0或1。它不建立运行块，仍以 FS=0 进入。已有 K42 的手工基址程序因而继续成立。非空描述先保存事实，所有 LOAD 读完后再核对其总内存范围属于一个 LOAD；有初值时，文件偏移还要与该 LOAD 中的相对位置一致。TLS 头可以排在数据 LOAD 之前，不能边读到它边错误断言覆盖段不存在。

最后检查 LOAD 页数加 TLS 运行页数不能超过为用户栈预留后的预算。所有结果仍放在局部 result，任何失败不改调用者的 view。用户 exec 的旧空间甚至还没进入分配阶段，超限模板应返回 −8，而非尝试访问错误 FS 或产生内核故障。

在程序头遍历中保存唯一 TLS 描述并计算布局，动态装载要求继续拒绝；此时只保存事实，尚不分配运行页。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/elf.c -->
<!-- before -->
```c
    if (!h.phnum || h.phnum > 16 || !fits(h.phoff, (uint64_t)h.phnum * h.phentsize, bytes)) // 程序头表也必须完整落在映像内。
        return "ELF program table is invalid"; // 有界数量保证后续乘法不溢出。
    struct airi_elf_view result = {.image=image, .bytes=bytes, .entry=h.entry}; // 所有结果先放局部，失败不破坏调用者旧状态。
    bool entry_found = false; // 最后要求入口确实落在文件提供的可执行内容。
    for (unsigned i = 0; i < h.phnum; ++i) { // 逐项读取已验证范围内的程序头。
        struct program_header p; // 文件可能未按宿主结构对齐，仍采用字节复制。
        airi_memcpy(&p, result.image + h.phoff + i * h.phentsize, sizeof(p)); // 单项来自固定程序表。
        if (p.type == 2 || p.type == 3 || p.type == 7) // PT_DYNAMIC、PT_INTERP和PT_TLS需要尚未实现的运行时。
            return "ELF requires a dynamic loader or TLS"; // 不忽略不可满足的装载要求。
        if (p.type == 0x6474e551 && (p.flags & 1)) // GNU_STACK请求可执行栈不在当前策略中。
            return "ELF executable stack is unsupported"; // 用户栈始终NX。
        if (p.type != 1) // 其他不参与装载的辅助头不被当作LOAD访问。
```
<!-- after -->
```c
    if (!h.phnum || h.phnum > 16 || !fits(h.phoff, (uint64_t)h.phnum * h.phentsize, bytes)) // 程序头表也必须完整落在映像内。
        return "ELF program table is invalid"; // 有界数量保证后续乘法不溢出。
    struct airi_elf_view result = {.image=image, .bytes=bytes, .entry=h.entry}; // 所有结果先放局部，失败不破坏调用者旧状态。
    bool tls_seen = false; // 一个静态ELF最多一份TLS模板，空头也不能重复。
    bool entry_found = false; // 最后要求入口确实落在文件提供的可执行内容。
    for (unsigned i = 0; i < h.phnum; ++i) { // 逐项读取已验证范围内的程序头。
        struct program_header p; // 文件可能未按宿主结构对齐，仍采用字节复制。
        airi_memcpy(&p, result.image + h.phoff + i * h.phentsize, sizeof(p)); // 单项来自固定程序表。
        if (p.type == 2 || p.type == 3) // PT_DYNAMIC与PT_INTERP仍需要尚未实现的动态运行时。
            return "ELF requires a dynamic loader"; // 不忽略不可满足的装载要求。
        if (p.type == 7) { // PT_TLS是初值模板，不在其p_vaddr原处当作当前线程变量使用。
            if (tls_seen || p.flags != 4 || p.file_bytes > p.memory_bytes || !fits(p.offset, p.file_bytes, bytes)) // 只接受一个可读模板且初值完整。
                return "ELF TLS file range or permissions are invalid"; // 不发布半份结果。
            tls_seen = true; // 后续遇到另一空/非空TLS都拒绝。
            if (!p.memory_bytes) { // LLD为无TLS程序保留一份全零空头，不需要运行块。
                if (p.file_bytes || p.address || p.offset || p.align > 1) // 仅接受本链接脚本明确产生的空描述。
                    return "ELF empty TLS template is invalid"; // 不靠空长度隐藏非法地址或对齐。
                continue; // 该程序仍以FS=0进入，不影响K42手工本地块。
            }
            uint64_t alignment = p.align ? p.align : 1; // ELF中0/1都表示没有额外对齐要求。
            if (p.memory_bytes > AIRI_ELF_TLS_LIMIT || alignment > 4096 || (alignment & (alignment - 1)) || (p.address % alignment) || (p.offset % alignment)) // 当前固定零偏移布局，对齐必须为有限2的幂。
                return "ELF TLS size or alignment is unsupported"; // 不接受需要超出当前块布局的偏斜模板。
            if (p.address < 0x10000 || p.address >= 0x10000000 || p.memory_bytes > 0x10000000 - p.address) // 模板仍只能属于受控用户映像范围。
                return "ELF TLS virtual range is invalid"; // 不从内核或堆区构造模板。
            unsigned block_align = alignment < 16 ? 16 : (unsigned)alignment; // TCB指针至少16字节对齐。
            unsigned pointer = ((unsigned)p.memory_bytes + block_align - 1) & ~(block_align - 1); // 运行块起点到TP的跨度。
            unsigned span = ((unsigned)p.memory_bytes + (unsigned)alignment - 1) & ~((unsigned)alignment - 1); // 链接器local-exec所用的TLS静态跨度。
            result.tls = (struct airi_elf_tls){p.address, p.offset, p.file_bytes, p.memory_bytes, alignment, pointer, pointer - span, (pointer + 16 + 4095) / 4096}; // TLS在TP前，TCB的首字指向TP自身。
            continue; // 不将同一模板再次作为一个独立LOAD段登记。
        }
        if (p.type == 0x6474e551 && (p.flags & 1)) // GNU_STACK请求可执行栈不在当前策略中。
            return "ELF executable stack is unsupported"; // 用户栈始终NX。
        if (p.type != 1) // 其他不参与装载的辅助头不被当作LOAD访问。
```

全部 LOAD 读完以后，再检查 TLS 的内存/文件对应关系与副本页预算，最后才发布 view。

<!-- replace: kernel/process/elf.c -->
<!-- before -->
```c
    }
    if (!result.count || !entry_found) // 没有LOAD或没有有效入口就不能启动。
        return "ELF entry is outside executable file contents"; // 不靠CPU故障验证基本文件格式。
    *view = result; // 唯一一次修改输出，旧结果在任何失败路径都保持不变。
    return NULL; // 后续装载者可以按已验证范围分配、复制并清BSS。
}
```
<!-- after -->
```c
    }
    if (!result.count || !entry_found) // 没有LOAD或没有有效入口就不能启动。
        return "ELF entry is outside executable file contents"; // 不靠CPU故障验证基本文件格式。
    if (result.tls.memory_bytes) { // 所有LOAD都已读完，TLS头可以出现在它们之前。
        bool contained = false; // 模板必须由一个普通可读LOAD完整提供，而非任意文件字节。
        for (unsigned i = 0; i < result.count; ++i) { // 最多八项，逐个核对对应关系。
            const struct airi_elf_segment *segment = &result.segments[i]; // 已验证用户范围与文件边界。
            if (result.tls.address < segment->address) // 用减法前先确认有非负偏移。
                continue; // 不能将负值回绕成小范围。
            uint64_t delta = result.tls.address - segment->address; // TLS模板在该段中的相对位置。
            if (delta > segment->memory_bytes || result.tls.memory_bytes > segment->memory_bytes - delta) // 总模板含零填充必须在段内。
                continue; // 不接受孤立或越界TLS描述。
            if (result.tls.file_bytes && (delta > segment->file_bytes || result.tls.file_bytes > segment->file_bytes - delta || result.tls.offset != segment->offset + delta)) // 有初值时VA和文件位置必须对应同一LOAD字节。
                continue; // 零初值只需要内存范围，不读取文件偏移处内容。
            contained = true; // 由一个完整合法段覆盖。
        }
        if (!contained || result.tls.pages > AIRI_ELF_PAGES - result.pages) // 运行副本也占用户预算，不能因模板已计页就漏算复制页。
            return "ELF TLS is outside LOAD or exceeds the user page budget"; // 真正分配前拒绝，旧view不变。
    }
    *view = result; // 唯一一次修改输出，旧结果在任何失败路径都保持不变。
    return NULL; // 后续装载者可以按已验证范围分配、复制并清BSS。
}
```

<a id="ctls-load"></a>
## 新根中的运行副本要通过它自己的页表初始化

ELF 检查已经把模板大小、初值文件位置和运行布局放进 view.tls；接下来才建立用户页。在现有 `kernel/process/user.c` 中完成下面四组局部替换，其余 K42 内容保留。先在动态堆起点之后加入独立 TLS 起点：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
```
<!-- after -->
```c
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_TLS 0x60000000ull // 静态TLS运行块不与ELF、动态堆和固定用户栈重叠。
```

再接入四份 ELF 的边界声明，以及五个可启动名称。ctls 与 ctls-reset 使用同一份文件，装载时通过不同的 argv[0] 进入两个分支；这些边界要等后面的程序、链接规则与包装全部完成后才有定义。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
extern const uint8_t airi_tls_big_elf_start[], airi_tls_big_elf_end[]; // 真正含超限PT_TLS的错误ELF。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
```
<!-- after -->
```c
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
    {"ctls-page", airi_ctls_page_elf_start, airi_ctls_page_elf_end}, // 两页运行块的真实边界。
    {"tls-too-big", airi_tls_big_elf_start, airi_tls_big_elf_end}, // 真实装载器必须拒绝，不进入用户main。
```

现在回到 prepare_elf。LOAD 和十六页用户栈已经建立，但新根还没有活动。非空 TLS 需要从0x60000000再建立一至两页 RW/NX 数据；owned_page 清零并登记所有权，随后只复制 file_bytes 初值，其余模板和填充保持零。目标根此时尚未活动，不能直接把这个用户 VA 当内核指针写入；copy_user 查询的是该目标根，经 HHDM 写对应物理页。

数据落在 USER_TLS 加 data_offset，TP 则在 USER_TLS 加 pointer_offset。将 TP 保存为 fs_base 后，还要在 TP 写两字 TCB，第一项等于 TP 自身。这样返回用户前恢复 FS，main 的第一条 TLS 访问就能先取得 self，再沿已链接的负偏移找到变量。构造失败仍清理整份新映像，不发布半初始化状态。

在原来的栈页循环与 initial_frame 之间接入这段构造；下面用这两处旧语句共同定位：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 最高页地址不变，向低地址增加十五页。
        if (!owned_page(p, USER_STACK - i * 4096ull, AIRI_VM_WRITE)) // 栈RW/NX，最低页之下仍无映射。
            goto failed; // 任意一页失败均回收整份新映像。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
```
<!-- after -->
```c
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 最高页地址不变，向低地址增加十五页。
        if (!owned_page(p, USER_STACK - i * 4096ull, AIRI_VM_WRITE)) // 栈RW/NX，最低页之下仍无映射。
            goto failed; // 任意一页失败均回收整份新映像。
    if (view.tls.memory_bytes) { // 无TLS程序继续FS=0，有模板才建立独立运行副本。
        for (unsigned i = 0; i < view.tls.pages; ++i) // 当前最多两页，含TP后的16字节TCB。
            if (!owned_page(p, USER_TLS + i * 4096ull, AIRI_VM_WRITE)) // 仍由用户页记录持有，RW/NX且fork逐页复制。
                goto failed; // 任何分配失败回滚整个新映像。
        uint64_t data = USER_TLS + view.tls.data_offset; // 小对齐模板可能在块首留下padding，保证负偏移相对TP正确。
        if (view.tls.file_bytes && !copy_user(p, (void *)(view.image + view.tls.offset), data, (size_t)view.tls.file_bytes, true)) // 按目标页查物理别名，初值可能跨页。
            goto failed; // 只复制filesz，memsz剩余保持owned_page的零填充。
        p->fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        uint64_t control[2] = {p->fs_base, 0}; // FS:0为self，第二字保留给后续自有运行库。
        if (!copy_user(p, control, p->fs_base, sizeof(control), true)) // TCB属于当前新用户根，不能直接写未活动用户VA。
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->fs_base); // 模板与运行位置分别可观察。
    }
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
```

fork 仍复制所有 owned 页面与保存的基址，所以继承当前修改后的 TLS 值，不从模板重置。成功 exec 准备新的运行页、复制原始初值，再换旧根；失败 exec 保持旧页与旧 FS。固定 TLS VA 不在动态堆范围，原 release_user 不会把它当作可自由归还的动态申请。

更新用户链接脚本。新增只读 PT_TLS 程序头，将 .tdata 和 .tbss 同时归入数据 LOAD 和 TLS 模板描述，普通 .data/.bss 仍保持原用途。段头的 PF_R 说明模板的读取语义，不是要求实际运行副本只读；副本必须能写当前线程变量。

这里的 . = ALIGN(64) 安排模板起点，真正的 PT_TLS 对齐由输入 TLS 节需求形成。ctls-char 只有一个对齐1的字节，输出头的 p_align 仍为1，所以布局计算不能偷用固定64。

<!-- write: labs/airios/program.ld -->
```ld
OUTPUT_FORMAT(elf64-x86-64) /* 生成独立静态用户ELF。 */
ENTRY(_start) /* 用户入口由自己的start.S提供。 */
PHDRS { /* 各权限段单独装载，避免共页权限冲突。 */
    text PT_LOAD FLAGS(5); /* 读、执行，不写。 */
    rodata PT_LOAD FLAGS(4); /* 只读常量。 */
    tls PT_TLS FLAGS(4); /* 静态本地变量的只读模板描述，普通LOAD仍承载模板字节。 */
    data PT_LOAD FLAGS(6); /* 读写数据和零填充，不执行。 */
}
SECTIONS { /* 固定低地址，当前不支持PIE重定位。 */
    . = 0x400000; /* 与用户装载范围一致的低半区。 */
    .text : { *(.text .text.*) } :text /* 用户入口和函数代码。 */
    . = ALIGN(4096); /* 下一权限段从新页开始。 */
    .rodata : { *(.rodata .rodata.*) } :rodata /* 文字与只读常量。 */
    . = ALIGN(4096); /* 不让常量与可写数据共页。 */
    .data : { *(.data .data.*) } :data /* 文件提供的初始化字节。 */
    . = ALIGN(64); /* 初值模板起点与当前支持的普通静态布局一致。 */
    .tdata : { *(.tdata .tdata.*) } :data :tls /* 带初值的线程本地模板。 */
    .tbss : { *(.tbss .tbss.*) } :data :tls /* 零填充TLS紧随初值，运行时单独复制。 */
    .bss : { *(.bss .bss.*) *(COMMON) } :data /* 只增加内存长度，装载器必须清零。 */
    /DISCARD/ : { *(.eh_frame*) *(.note*) } /* 本节不提供异常展开或辅助note消费。 */
}
```

<a id="ctls-programs"></a>
## 用普通 C 本地变量覆盖创建、fork 与两种 exec

新建 ctls.c。initialized 有初值42，aligned 有初值7且要求64字节对齐，zero 为73字节零数组。volatile 要求每次实际读写，避免编译器用一个 GPR 值替代整个循环；它不提供线程同步或原子性。本节父子仍分属不同地址空间，不在共享变量上竞争。

main 先核对全部初值、零尾部、地址对齐和 TCB self，然后把值改为99/14再 fork。父子必须先继承这些已修改值，之后才分别改为111/222并纯计算跨时钟。超限真实 ELF 的 exec 返回−8后，旧本地值继续存在。孩子成功 exec 回同一 ELF，以 ctls-reset 的 argv[0] 进入只核对默认值的分支；父 wait 后仍有自己的111/112。

父最后继续 exec ctls-char、ctls-page，使大小模板都沿同一个真实 shell 命令完成。成功 exec 不回旧 C 调用链，结束代码由最后的 main 给出。

<!-- write: labs/airios/ctls.c -->
```c
#include <airi_libc.h> // 编译器生成TLS地址；用户主程序不再手工分配或设置FS。
_Thread_local volatile uint64_t initialized = 42; // .tdata提供每个新映像的相同初值。
_Thread_local volatile unsigned char zero[73]; // .tbss只有内存长度，必须真正零填充。
_Alignas(64) _Thread_local volatile uint64_t aligned = 7; // TLS对齐约束高于普通TCB的16字节。
static int initial(void) { // main在任何修改前观察装载器创建的本地副本。
    if (initialized != 42 || aligned != 7 || (uintptr_t)&aligned % 64) // 初值、实际地址与编译器对齐分别核对。
        return 1; // 不能只读PT_TLS描述却没正确复制。
    for (unsigned i = 0; i < sizeof(zero); ++i) // 检查整个零填充跨度，不只看第一个字节。
        if (zero[i]) // 旧用户内容不能进入新TLS块。
            return 2; // 明确失败。
    long base = airi_fs_base(); // 必须已经在_start调用main以前设好。
    if (base < 0x60000000 || base > 0x60001000 || base % 16 || *(uintptr_t *)(uintptr_t)base != (uintptr_t)base) // TP后的TCB首字为self。
        return 3; // 本节只支持固定静态块和local-exec模型。
    return 0; // 初始副本与默认布局完整成立。
}
int main(int argc, char **argv) { // 一份静态ELF以目录名区分普通工作与exec重置观察。
    int error = initial(); // 编译器实际使用FS负偏移读取，不调用手工TLS初始化函数。
    if (error) // 初始环境错误保留不同退出位置。
        return 210 + error; // shell可继续报告错误。
    if (argc == 1 && strlen(argv[0]) > 4 && argv[0][4] == '-') { // ctls-reset是同一ELF的新独立装载，不是旧调用返回。
        airi_puts("[CTLS_EXEC_RESET_OK] initialized=42 zero=73 aligned=64 self=valid\n"); // 新模板重新生成默认本地数据。
        return 0; // 原父按同一孩子PID取得结果。
    }
    initialized = 99; zero[0] = 14; // fork应继承当前修改后的TLS，而非再次从文件初值42开始。
    long child = airi_fork(); // 普通地址空间复制也包括固定TLS运行页。
    if (child < 0 || initialized != 99 || zero[0] != 14) // 父子返回后都看到调用点的旧值。
        return 214; // 不把fork做成exec式初始化。
    uint64_t expected = child ? 111 : 222; // 父子采用不同本地数据但相同的符号与虚拟地址。
    initialized = expected; aligned = expected + 1; // 当前进程的TLS物理页面分别拥有。
    uint64_t start = airi_ticks(); // 观察真实抢占窗口。
    do { // 每轮只有用户计算，时钟可以任意打断。
        for (unsigned i = 0; i < 5000000; ++i) // volatile使编译器每次实际读TLS变量。
            if (initialized != expected || aligned != expected + 1 || zero[0] != 14) // 保持各自值，普通GPR副本不能代替全部读。
                return 215; // 跨身份串扰即失败。
    } while (airi_ticks() - start < 20); // 二十个实际tick，非主动yield造出的轮转。
    if (airi_exec("tls-too-big") != -8 || initialized != expected || zero[0] != 14) // 超限PT_TLS必须在分配前拒绝，旧FS与值不变。
        return 216; // 不是仅在宿主解析器里跑合成错误。
    if (!child) { // 孩子成功替换同一份ELF，以新名称进入重置分支。
        airi_puts("[CTLS_CHILD_OK] fork=modified-inherited preempt=private invalid_elf=preserved\n"); // 实际C本地变量检查已完成。
        airi_exec("ctls-reset"); // 新副本应重新为42、7和全零。
        return 217; // 成功exec不回旧程序。
    }
    int status = 1; // 同一孩子在新ELF中的实际退出结果。
    if (airi_waitpid(child, &status) != child || status || initialized != 111 || aligned != 112) // 父自己的副本继续活着。
        return 218; // 等待与exec不能覆盖父TLS。
    airi_puts("[CTLS_USER_OK] c11=thread_local tdata=42 tbss=73 align=64 fork=copy exec=reset invalid=unchanged\n"); // 所有机制均完成后发布。
    airi_exec("ctls-char"); // 再覆盖仅一字节、对齐1的真实链接布局。
    return 219; // 父成功exec后由新main返回0。
}
```

新建 ctls-char.c。这份独立 ELF 只含一字节 TLS，要求 TP=0x60000010，而 &only=TP−1，能捕获“小模板复制在块首却让 TP 对齐16”的错误。它实际修改并读回，再执行下一份程序。

<!-- write: labs/airios/ctls-char.c -->
```c
#include <airi_libc.h> // 同一静态运行入口，不手工初始化TLS。
_Thread_local volatile unsigned char only = 7; // 唯一TLS变量迫使模板为1字节、对齐1。
int main(void) { // TLS数据应在16字节对齐TP之前最后一个字节。
    long base = airi_fs_base(); // 编译器地址计算还会通过FS:0读取self。
    if (base != 0x60000010 || (uintptr_t)&only != (uintptr_t)base - 1 || only != 7) // 不能错误地从块首复制后仅把TP向上挪15字节。
        return 220; // 小对齐不是64对齐测试的同义重复。
    only = 9; // 实际写入编译器解析出的负偏移。
    if (only != 9) // 硬件用户写必须能读回。
        return 221; // 不以模板常量替代访问。
    airi_puts("[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9\n"); // 真实小模板布局正确。
    airi_exec("ctls-page"); // 最后覆盖4KiB纯零模板与第二页TCB。
    return 226; // 成功exec不回到这一份小模板。
}
```

新建 ctls-page.c。纯零数组正好4096字节，file_bytes为0，TCB另占第二页。程序读取全数组，再写两个端点，并读取第二页的 self，确认文件长度、模板长度和运行页面数三者确实分别处理。

<!-- write: labs/airios/ctls-page.c -->
```c
#include <airi_libc.h> // 模板达到上限且没有文件初值，TCB必须另有映射。
_Thread_local volatile unsigned char page[4096]; // 真实.tbss整页，PT_TLS.filesz为0而memsz为4096。
int main(void) { // 编译器仍用local-exec地址，不手工设置FS。
    long base = airi_fs_base(); // TP应紧随4096字节本地数据，位于第二页起点。
    if (base != 0x60001000 || (uintptr_t)&page[0] != 0x60000000 || *(uintptr_t *)(uintptr_t)base != (uintptr_t)base) // TCB首字也必须可读且为self。
        return 223; // 只映射模板一页而漏掉TCB会在这里暴露。
    for (unsigned i = 0; i < sizeof(page); ++i) // 零初值不需要文件里提供4096个字节。
        if (page[i]) // 检查全范围而非仅首尾。
            return 224; // 不能复用旧TLS内容。
    page[0] = 0x31; page[4095] = 0x79; // 两个边界值都属于TLS数据，不覆盖下一页TCB。
    if (page[0] != 0x31 || page[4095] != 0x79) // 实际读回确认边界寻址。
        return 225; // 明确报告失败。
    airi_puts("[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all\n"); // 最大模板和额外TCB页均已实际访问。
    return 0; // 原shell的ctls命令至此结束。
}
```

新建 tls-too-big.c。4097字节 volatile 本地数组保留在真实编译结果中，用于 guest 装载器的拒绝路径。main 不应开始执行；正确结果是调用者拿到 exec 的−8，而不是该 main 的222。

<!-- write: labs/airios/tls-too-big.c -->
```c
#include <airi_libc.h> // 这份ELF只用于装载边界，正常实现不能进入main。
_Thread_local volatile unsigned char excess[4097]; // 比本轮模板上限4096多一个字节。
int main(void) { // 若装载器忽略PT_TLS，不能以无输出返回冒充拒绝。
    excess[0] = excess[4096]; // 两个端点都被真实使用，编译器不能删掉超限对象。
    return 222; // 用户工作负载应收到exec的-8，而不是此退出码。
}
```

把四份完整静态 ELF 加入 images.S，各自宏路径由 Makefile 提供。目录项没有把它们直接链接成内核可调用函数，仍用已有 ELF 检查、准备与提交。

在 `labs/airios/images.S` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* C本地变量探针的真实静态ELF。 */
.global airi_ctls_elf_start, airi_ctls_elf_end /* 一个映像供普通与reset两个目录名。 */
airi_ctls_elf_start: /* 完整程序头含PT_TLS。 */
.incbin AIRI_CTLS_ELF /* 来自本配置交叉链接产物。 */
airi_ctls_elf_end: /* 用户文件右端。 */
.balign 8 /* 单字节TLS布局单独编译。 */
.global airi_ctls_char_elf_start, airi_ctls_char_elf_end /* 不混入普通探针的64字节对齐要求。 */
airi_ctls_char_elf_start: /* 唯一TLS符号的ELF。 */
.incbin AIRI_CTLS_CHAR_ELF /* 文件大小与PT_TLS长度并不相等。 */
airi_ctls_char_elf_end: /* 映像范围结束。 */
.balign 8 /* 错误ELF仍作为只读字节输入校验器。 */
.global airi_tls_big_elf_start, airi_tls_big_elf_end /* 装载应拒绝这份超限模板。 */
airi_tls_big_elf_start: /* 不把该程序当内核故障执行。 */
.incbin AIRI_TLS_BIG_ELF /* 明确含4097字节TLS对象。 */
airi_tls_big_elf_end: /* 输入边界供ELF校验。 */

.balign 8 /* 最大TLS模板的纯零ELF。 */
.global airi_ctls_page_elf_start, airi_ctls_page_elf_end /* 独立运行页与TCB页的实际程序。 */
airi_ctls_page_elf_start: /* 没有TLS文件初值，但有4096字节内存模板。 */
.incbin AIRI_CTLS_PAGE_ELF /* 真实链接器产生的PT_TLS，而非手工标记。 */
airi_ctls_page_elf_end: /* 校验器可读输入的右端。 */
```

更新 Makefile，在 USER_FLAGS 加 -ftls-model=local-exec。当前没有动态模块，偏移由 LLD 静态解析；若出现未解析的 __tls_get_addr 依赖，独立 ELF 符号检查会拒绝，不能用一个占位返回值蒙混。四个用户目标各自只有一个 main，继续复用自有入口与用户库。

USER_FLAGS 明确选择 local-exec，其他用户策略保留。

<!-- replace: Makefile -->
<!-- before -->
```make
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -mno-avx -MMD -MP -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
```
<!-- after -->
```make
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -mno-avx -ftls-model=local-exec -MMD -MP -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
```

在 Makefile 末尾追加四个独立 ELF 与包装规则。

<!-- append: Makefile -->
```make

# 三份静态TLS用户映像保持各自main，复用普通C入口和自有库。
$(OUT)/user/ctls.elf: $(USER_COMMON) $(OUT)/user/ctls.o labs/airios/program.ld
# local-exec重定位由LLD按PT_TLS静态跨度解析，内核不运行动态链接器。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/ctls.o -o $@
# 单字节模板验证小于TCB对齐时的数据落点。
$(OUT)/user/ctls-char.elf: $(USER_COMMON) $(OUT)/user/ctls-char.o labs/airios/program.ld
# 不能与ctls.o一起链接，否则模板会混入大对齐变量。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/ctls-char.o -o $@
# 只编译超限样本，装载器应在用户运行前返回-8。
$(OUT)/user/tls-too-big.elf: $(USER_COMMON) $(OUT)/user/tls-too-big.o labs/airios/program.ld
# 完整ELF用于真实guest失败事务，不用宿主字符串模拟拒绝。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls-too-big.o -o $@
# 包装等待三份实际产物。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls.elf $(OUT)/user/ctls-char.elf $(OUT)/user/tls-too-big.elf
# 路径只供当前配置的.incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_ELF='"$(OUT)/user/ctls.elf"' -DAIRI_CTLS_CHAR_ELF='"$(OUT)/user/ctls-char.elf"' -DAIRI_TLS_BIG_ELF='"$(OUT)/user/tls-too-big.elf"'

# 四KiB纯零TLS需要第二页TCB，另立ELF不混入其他TLS初值。
$(OUT)/user/ctls-page.elf: $(USER_COMMON) $(OUT)/user/ctls-page.o labs/airios/program.ld
# 与其他用户相同入口和local-exec模型。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/ctls-page.o -o $@
# 包装等待本配置最大模板映像。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls-page.elf
# 单独路径宏只由包装汇编使用。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_PAGE_ELF='"$(OUT)/user/ctls-page.elf"'
```

<a id="ctls-errors"></a>
## 用合成输入检查坏描述，再用 guest 运行实际地址

更新 elf_test.c。正常输入在原两份 LOAD 上加一份 TLS，观察73字节、对齐16的布局；小于TCB对齐的一字节、零对齐规范化、4096字节两页与空头分别检查。失败输入覆盖截断初值、回绕偏移、过大长度、非法对齐、孤立模板、不对应的文件位置、重复描述和漏算副本预算，每次要求旧 view 的全部字节保持。

最后将 TLS 头移动到两份 LOAD 之前，确认合法顺序不会被误拒。宿主只验证这些输入事实，不执行 FS 或用户切换；真正初值、零尾部、负偏移、fork 和 exec 仍由前面的用户程序实测。

在已有有效输入与拒绝检查助手之后，加入 TLS 输入和单字段破坏助手。

<!-- replace: tests/unit/elf_test.c -->
<!-- before -->
```c
    put(offset, value, bytes); // 只修改这一种字段事实。
    rejected(sizeof(image)); // 要求拒绝并保持旧view不变。
}
int main(void) { // 本程序只验证解析器，不替代guest的BSS和权限运行。
    fixture(); // 两段、三数据页、有效入口。
    struct airi_elf_view view; // 接收完整已验证事实。
```
<!-- after -->
```c
    put(offset, value, bytes); // 只修改这一种字段事实。
    rejected(sizeof(image)); // 要求拒绝并保持旧view不变。
}
static void tls_fixture(void) { // 在原两段合法映像上加入独立PT_TLS，不把它当第三个LOAD。
    fixture(); // LOAD布局与初值都来自原有受控输入。
    put(56, 3, 2); // 程序头新增第三项。
    put(176, 7, 4); put(180, 4, 4); // PT_TLS只读模板描述。
    put(184, 0x2000, 8); put(192, 0x401000, 8); // 文件和VA与数据LOAD对应同一字节。
    put(208, 4, 8); put(216, 73, 8); put(224, 16, 8); // 四字节初值、73字节总长与16字节对齐。
}
static void tls_corrupt(unsigned offset, uint64_t value, unsigned bytes) { // 每种TLS错误独立观察，避免被旧错误遮蔽。
    tls_fixture(); put(offset, value, bytes); // 从完整合法模板只改变一个字段。
    rejected(sizeof(image)); // 必须明确拒绝并保留调用者旧输出的全部字节。
}
static void tls_cases(void) { // 主机只核对元数据与布局，真实FS访问另在guest执行。
    struct airi_elf_view view; // 接收通过后的明确事实。
    tls_fixture(); // 普通带初值和零尾部模板。
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 合法TLS不能继续沿旧策略拒绝。
    assert(view.pages == 3 && view.tls.file_bytes == 4 && view.tls.memory_bytes == 73); // LOAD预算和运行副本长度分别记录。
    assert(view.tls.pointer_offset == 80 && view.tls.data_offset == 0 && view.tls.pages == 1); // 73按16向上舍入为80，TCB仍在同页。
    put(208, 1, 8); put(216, 1, 8); put(224, 1, 8); // 只有一字节且只要求1字节对齐。
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 小于TCB对齐的模板仍合法。
    assert(view.tls.pointer_offset == 16 && view.tls.data_offset == 15 && view.tls.pages == 1); // 变量位于TP-1，不能从块首错误初始化。
    put(224, 0, 8); // ELF的零对齐规范化为1，不参与除零。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && view.tls.alignment == 1); // 同一布局无需额外限制。
    tls_fixture(); put(208, 0, 8); put(216, 4096, 8); put(224, 4096, 8); // 全零模板达到本轮最大长度，TCB另占下一页。
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 文件里不需要放4096个零字节。
    assert(view.tls.pointer_offset == 4096 && view.tls.pages == 2 && !view.tls.data_offset); // 第二页必须计入用户预算。
    tls_fixture(); put(184, 0, 8); put(192, 0, 8); put(208, 0, 8); put(216, 0, 8); put(224, 0, 8); // LLD无TLS程序产生空描述。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && !view.tls.memory_bytes && !view.tls.pages); // 不改变旧无TLS程序入口。
    tls_corrupt(208, 74, 8); // 初值超过总长度或LOAD文件字节，拒绝。
    tls_corrupt(184, UINT64_MAX - 1, 8); // 输入文件范围不能回绕。
    tls_corrupt(216, 4097, 8); // 比明确模板上限多一个字节。
    tls_corrupt(216, UINT64_MAX, 8); // 极大memsz在布局加法前拒绝。
    tls_corrupt(224, 3, 8); // 非2幂对齐。
    tls_corrupt(224, 8192, 8); // 超出本轮单页对齐能力。
    tls_corrupt(192, 0x401001, 8); // 模板起点不符合声明的零偏移对齐。
    tls_corrupt(180, 6, 4); // PT_TLS模板头必须只读，不直接采用其W权限。
    tls_corrupt(192, 0x500000, 8); // 地址合法但不属于任何LOAD，不能接受孤立模板。
    tls_fixture(); put(224, 1, 8); put(184, 0x2001, 8); put(208, 1, 8); // 文件范围可读但与模板VA不是同一LOAD字节。
    rejected(sizeof(image)); // 不把任意文件偏移当可执行映像的TLS初值。
    tls_fixture(); put(56, 4, 2); put(232, 7, 4); put(236, 4, 4); // 再加一份空TLS头也不能绕过唯一性。
    rejected(sizeof(image)); // 不静默选最后一个模板。
    tls_fixture(); put(160, (AIRI_ELF_PAGES - 1ull) * 4096, 8); // LOAD本身刚好占满栈以外的预算。
    rejected(sizeof(image)); // TLS副本还有一页，不能因模板属于LOAD就漏算。
    tls_fixture(); uint8_t head[56]; // 测试TLS出现在其覆盖LOAD之前的合法输入。
    airi_memcpy(head, image + 176, 56); // 先保存TLS头，不丢失原内容。
    airi_memmove(image + 120, image + 64, 112); // 两份LOAD整体后移一项，确实使用重叠移动。
    airi_memcpy(image + 64, head, 56); // TLS成为第一个程序头。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && view.count == 2 && view.tls.memory_bytes == 73); // 容纳检查在所有LOAD完成后执行。
}
int main(void) { // 本程序只验证解析器，不替代guest的BSS和权限运行。
    fixture(); // 两段、三数据页、有效入口。
    struct airi_elf_view view; // 接收完整已验证事实。
```

在原来的正常/错误 ELF 检查之后，依次接入 TLS 布局、拒绝与头顺序观察。

<!-- replace: tests/unit/elf_test.c -->
<!-- before -->
```c
    corrupt(136, 0x400000, 8); // 两段共享同一页必须拒绝。
    corrupt(24, 0x401000, 8); // 入口落入可写NX数据段。
    corrupt(64, 3, 4); // PT_INTERP不能被静默忽略。
    corrupt(64, 7, 4); // 未实现TLS也不能当普通静态程序。
    fixture(); // 大BSS只有memsz增长，文件本身不需要装下零填充字节。
    put(160, (AIRI_ELF_PAGES - 1ull) * 4096, 8); // 一代码页加其余数据页，刚好达到ELF预算。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && view.pages == AIRI_ELF_PAGES); // 栈预留之后的精确上界仍可接受。
    put(160, AIRI_ELF_PAGES * 4096ull, 8); // 总段页数比预算多一页。
    rejected(sizeof(image)); // 失败保持上一份输出，不开始物理分配。
    puts("PASS elf: target, bounds, overflow, permissions, overlap, entry, budget, unchanged output"); // 所有失败与正常路径均成立。
    return 0; // 正常退出宿主观察。
}
```
<!-- after -->
```c
    corrupt(136, 0x400000, 8); // 两段共享同一页必须拒绝。
    corrupt(24, 0x401000, 8); // 入口落入可写NX数据段。
    corrupt(64, 3, 4); // PT_INTERP不能被静默忽略。
    corrupt(64, 7, 4); // 原RX代码头不能直接改名TLS绕过模板权限与入口规则。
    fixture(); // 大BSS只有memsz增长，文件本身不需要装下零填充字节。
    put(160, (AIRI_ELF_PAGES - 1ull) * 4096, 8); // 一代码页加其余数据页，刚好达到ELF预算。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && view.pages == AIRI_ELF_PAGES); // 栈预留之后的精确上界仍可接受。
    put(160, AIRI_ELF_PAGES * 4096ull, 8); // 总段页数比预算多一页。
    rejected(sizeof(image)); // 失败保持上一份输出，不开始物理分配。
    tls_cases(); // 静态TLS正常、小对齐、边界、恶意元数据与输出原子性。
    puts("PASS elf: target, bounds, overflow, permissions, overlap, entry, budget, static TLS layout/bounds/ownership, unchanged output"); // 所有失败与正常路径均成立。
    return 0; // 正常退出宿主观察。
}
```

shell_test 在原 tls 命令后输入 ctls，等待最后一个映像退出，再要求所有默认值、孩子继承、小模板、最大模板和坏 ELF 拒绝标记来自本次命令。原 simd、手工FS、容量、孤儿、设备与服务路径继续运行，静态 C TLS 不能破坏没有 TLS 的程序。

在手工 tls 后输入 ctls，等待最后一个映像结束，再检查本次模板、继承、重置与边界记录。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                for identity in (tls_parent.group(1),tls_fork.group(1)): # 二者都实际经过PIT窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',tls_output) # exec仍使用同一身份累计值。
                    assert ended and int(ended.group(1))>=2 # 主动yield或单身份内存比较不算抢占证据。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                for identity in (tls_parent.group(1),tls_fork.group(1)): # 二者都实际经过PIT窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',tls_output) # exec仍使用同一身份累计值。
                    assert ended and int(ended.group(1))>=2 # 主动yield或单身份内存比较不算抢占证据。
                offset=len(text); wire.sendall(b'ctls\n'); text=wait('[SHELL_COMMAND_DONE] name=ctls code=0',offset) # 编译器_Thread_local必须在main前建立，不由宿主注入值。
                ctls_output=text[offset:] # 固定本次命令的实际记录。
                assert '[CTLS_USER_OK] c11=thread_local tdata=42 tbss=73 align=64 fork=copy exec=reset invalid=unchanged' in ctls_output # 父子修改后fork、真正抢占与坏PT_TLS保持旧映像。
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

完整序列结束后，结果记录加入 compiler_static_tls。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

慢盘观察还需要明确限速器的初始状态。QEMU设置1 IOPS后可能立即放行首笔请求，孩子延后计算的输出便可能落在父两次CQ等待之间，即使设备和调度都正确，原来的严格等待区间断言仍会失败。storage_test先在第三次启动中设置限速，再通过同一guest执行一次真实run recover，等读回及清理完整结束，消耗首笔额度；随后发run async并等待第二次命令计数。

这个准备请求只读已提交记录，不改变恢复数据，不用宿主sleep伪造设备时序。原断言仍要求另一个用户的进展发生在父同一次CQ登记与唤醒之间，仍使用真实QMP限速和真实used完成。下面在脚本中接入准备请求与对应命令计数。

在限速设置后、目标命令前接入一次真实 recover，并按已发送命令数等待本次控制台完成。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
                        limits = {'device':'airi-disk', 'bps':0, 'bps_rd':0, 'bps_wr':0, 'iops':1, 'iops_rd':0, 'iops_wr':0} # 明确单IOPS且无额外突发，不限制吞吐字节。
                        qmp_execute(qmp, 'block_set_io_throttle', limits) # QEMU实际限速，不在guest里伪造完成延迟。
                        (work / 'async-throttle.json').write_text(json.dumps(limits, indent=2) + '\n') # 只在QMP接受后记录本次配置。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', offset) # 等命令与用户会话全部收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                    return text[offset:] # 交出本次请求的完整真实区间，finally仍先回收QEMU。
                finally: # 正常和错误均收回设备与CPU。
```
<!-- after -->
```python
                        limits = {'device':'airi-disk', 'bps':0, 'bps_rd':0, 'bps_wr':0, 'iops':1, 'iops_rd':0, 'iops_wr':0} # 明确单IOPS且无额外突发，不限制吞吐字节。
                        qmp_execute(qmp, 'block_set_io_throttle', limits) # QEMU实际限速，不在guest里伪造完成延迟。
                        (work / 'async-throttle.json').write_text(json.dumps(limits, indent=2) + '\n') # 只在QMP接受后记录本次配置。
                    command_count = 1 # 普通会话只发送本次目标命令。
                    if slow: # 限速器可能放行第一笔，先用真实只读请求消耗初始额度。
                        warm_offset = len(text) # 不使用启动演示的旧恢复输出。
                        wire.sendall(b'run recover\n') # 同一guest实际读取已提交记录，不修改正式日志。
                        text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', warm_offset) # 完整读回和会话清理后再发目标。
                        assert '[RECOVERY_USER_OK]' in text[warm_offset:] # 初始额度由真实设备读取消耗，不靠宿主sleep猜测。
                        command_count = 2 # async是同一新CPU的第二条监控命令。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait(f'[CONSOLE_UPDATE] view=0 commands={command_count} page=0', offset) # 按实际已发送命令数等待完整收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                    return text[offset:] # 交出本次请求的完整真实区间，finally仍先回收QEMU。
                finally: # 正常和错误均收回设备与CPU。
```

存储观察完成后，结果记录再标明初始额度由真实恢复请求消耗。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
        if registered.start() < progress < wake: # 子任务进展发生在同一次父调用等待期间。
            blocked = True # 不是凭QEMU仍运行推断并发。
    assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 真设备接收、完成和其他用户继续必须同时成立。
    result = {'passed':True, 'profile':profile, 'boots':3, 'restart_boots':2, 'recovered_records':1, 'throttled_async_iops':1, 'other_user_during_cq_wait':True, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage sessions: two-boot recovery + QMP-throttled CQ wait with other user progress') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
```
<!-- after -->
```python
        if registered.start() < progress < wake: # 子任务进展发生在同一次父调用等待期间。
            blocked = True # 不是凭QEMU仍运行推断并发。
    assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 真设备接收、完成和其他用户继续必须同时成立。
    result = {'passed':True, 'profile':profile, 'boots':3, 'restart_boots':2, 'recovered_records':1, 'throttled_async_iops':1, 'other_user_during_cq_wait':True, 'throttle_warmed_by_real_recovery':True, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage sessions: two-boot recovery + QMP-throttled CQ wait with other user progress') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
```

<a id="ctls-observe"></a>
## 在文件、指令和输出中追踪同一个变量

全部修改完成后运行：

<!-- run -->
```sh
./airios build # 链接器形成PT_TLS与已解析的local-exec偏移。
./airios test # 实际运行编译器TLS及原有全部诊断、输入、服务和存储路径。
```

Mac 仍选旧 Python 时给命令前加 `PATH=/opt/homebrew/bin:$PATH`。交互 `./airios run`，输入 `run sh` 后输入 `ctls`。本轮初次装载应看到：

```text
[ELF_TLS] bytes=153 initialized=72 alignment=64 pages=1 data=0x60000000 tp=0x600000c0
```

父子对超限程序各有一次明确拒绝，孩子成功替换后重新看到同样的默认模板：

```text
[ELF_REJECTED] ELF TLS size or alignment is unsupported
[CTLS_CHILD_OK] fork=modified-inherited preempt=private invalid_elf=preserved
[CTLS_EXEC_RESET_OK] initialized=42 zero=73 aligned=64 self=valid
[CTLS_USER_OK] c11=thread_local tdata=42 tbss=73 align=64 fork=copy exec=reset invalid=unchanged
[ELF_TLS] bytes=1 initialized=1 alignment=1 pages=1 data=0x6000000f tp=0x60000010
[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9
[ELF_TLS] bytes=4096 initialized=0 alignment=16 pages=2 data=0x60000000 tp=0x60001000
[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all
[SHELL_COMMAND_DONE] name=ctls code=0
```

PID 和抢占 tick 仍随实际运行分配，父子输出也可能交错。最后回 shell，可以继续 tls，确认无模板程序仍从 FS=0 开始。退出 shell 后原会话核对页、活堆和FD恢复，运行TLS页不会永久留在页池外。

用 LLVM 的 llvm-readelf -lW 检查 build/normal/user/ctls.elf，可看到 TLS 头的 filesz=0x48、memsz=0x99、align=0x40，以及与数据 LOAD 对应的文件位置。llvm-objdump -r 查看 ctls.o 时，变量处仍是 TPOFF32 重定位；对最终 ctls.elf 使用 -d 则可看到 FS:0 读取及 −0xc0、−0x80、−0x70 等已解析偏移。模板地址0x402000与实际变量地址0x60000000各司其职。

当前支持单个静态模块，TCB只有 self 和保留字，无DTV、动态TLS解析、构造器、析构器或共享地址空间线程。此处完成编译器和内核的最小交接，随后可以用 `_Thread_local` 实现错误状态，再继续线程与futex；Linux实验和真实板部署仍独立记录。

