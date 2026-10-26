# K21 · 装载独立静态 ELF，进入自己的用户 C 运行时

K20 已能把一页指令放进 ring3，代码位置、栈位置和字节数量却都由内核预先约定。现在把用户程序写成普通 C：除了指令，它还有 initialized 的初值、尚无初值的 zeroed 数组和 main 的参数。只复制一段汇编字节，不能告诉内核这些对象需要哪些内存和权限。

用户构建因此先产出独立 ELF，由程序头描述装载要求。内核按段分配页，复制文件提供的字节，其余跨度保留零，再把 argc/argv 放进初始用户栈。CPU 随后从用户 _start 进入 main，字节函数和输出包装则由这份 ELF 自己链接的 libc 子集提供。

用户程序核对可写初始数据、跨页8192字节BSS、程序名、重叠移动和实际输出，最终从main返回0，由用户运行时调用AiriOS exit。原四个隔离演示继续保留回归。本节仍顺序运行单个上下文，没有fork/exec/wait或多个用户任务轮转；下节在这个装载边界上继续建立进程语义。

先写独立ELF检查器与主机观察，再写用户库、程序入口和链接布局，最后更新内核装载、构建及启动观察。所有新文件完成后统一运行。libc和构建脚本都是完整自有实现，不从宿主或内核偷连成品。

<a id="elf-file-memory"></a>
## 文件里的字节不等于装载后的内存

zeroed 需要在进程中占 8192 字节，却不必把 8192 个零都存进文件。内核必须按描述创建 RAM，不能用文件长度推断运行内存。这就是 ELF 程序头中 p_filesz 与 p_memsz 分别存在的原因：前者给出文件提供多少字节，后者给出进程应有多少字节，超出的尾部必须清零，C 未显式初始化的静态对象通常放在那里。

ELF 文件头还给机器类型、入口和程序头表位置。装载器读取 PT_LOAD，不从节名猜哪些内容需要运行。每个 LOAD 的 p_offset 是文件起点，p_vaddr 是用户虚拟目标。p_paddr 不成为用户物理目标：物理页仍从内核页池分配，否则输入文件就能要求覆盖内核。

本次 initialized 的值来自 .data 文件字节，zeroed 的值来自零填充。段的起止不一定对齐页边界，映射要向外覆盖整页；复制却仍从准确 p_vaddr 开始，只复制 filesz。先清零所有数据页，再覆盖文件部分，既保住 BSS，也让页边缘保持零。

首版只接受 ELF64、小端、x86-64、静态 ET_EXEC，最多 16 个程序头、8 个 LOAD、63 个段数据页，第 64 页留给初始栈。用户段在低 256MiB，动态装载器、TLS、可执行栈、RWX 与共享边缘页的 LOAD 布局都拒绝。这是当前装载合同，范围外不代表所有 ELF 都非法，而是本实现不支持。

**新建 `include/airi/elf.h`。** view只借用输入直到复制结束，失败不得改变调用者原view。

<!-- write: include/airi/elf.h -->
```c
#ifndef AIRI_ELF_H // 静态ELF解析边界只声明一次。
#define AIRI_ELF_H // 包含保护。
#include <stdbool.h> // 校验器使用布尔范围与入口状态。
#include <airi/base.h> // 固定宽度字段与自有字节复制。
#define AIRI_ELF_SEGMENTS 8u // 教学程序最多八个LOAD段。
#define AIRI_ELF_PAGES 63u // 留出第64页给初始用户栈。
struct airi_elf_segment { // 从外部字节校验后复制出的段事实。
    uint64_t address, offset, file_bytes, memory_bytes; // 用户VA、映像偏移、初始化长度和总内存长度。
    unsigned flags; // 原ELF PF_R/PF_W/PF_X，不能当作x86 PTE直接写入。
};
struct airi_elf_view { // 只借用输入映像，装载完成前映像必须持续有效。
    const uint8_t *image; // 不拥有也不修改这些输入字节。
    size_t bytes; // 输入映像的实际可读取长度。
    uint64_t entry; // 入口必须位于可执行段的文件内容中。
    unsigned count, pages; // 已验证LOAD段数与向外舍入后的总页数。
    struct airi_elf_segment segments[AIRI_ELF_SEGMENTS]; // 每项已通过范围、权限和重叠检查。
};
const char *airi_elf_inspect(const void *image, size_t bytes, struct airi_elf_view *view); // NULL表示通过；失败不修改输出view。
#endif // 结束包含保护。
```

<a id="elf-validation"></a>
## 在分配之前校验整份输入

拿到这些装载要求后，先核对整份文件，再分配页。若前几段已分配，最后才发现入口或长度非法，就需要撤销半个映像；独立解析器先产出一个经过核实的 view，让资源所有者随后照它构造。

文件地址可能未对齐，因此先确认最小头可读，再 memcpy 到对齐局部结构，不直接强转输入。静态断言固定文件头 64 字节、程序头 56 字节。范围检查 fits 使用 offset≤bytes 和 length≤bytes−offset，避免 offset+length 已回绕却通过比较。

对每个 LOAD，依次检查 filesz≤memsz、文件跨度、虚拟右端、PF 权限、页内偏移以及 p_align 的 2 次幂要求。随后比较向外舍入的页区间：即使两个原始字节区间不重叠，共享边缘页也可能要求相反权限，本章直接拒绝。入口还必须在可执行段的文件内容内，不能落到 NX 数据或纯 BSS 尾部。

这些检查先累积到局部 result，直到所有程序头成立才一次写回 view。失败不改变调用者的原 view；成功的 view 仍只是借用输入的描述，没有执行代码，也没有分配目标 RAM。调用者必须让输入活到文件字节复制完成。

**新建 `kernel/process/elf.c`。** 不访问节表，不执行输入代码，不根据p_paddr写物理内存。

<!-- write: kernel/process/elf.c -->
```c
#include <airi/elf.h> // 静态x86-64用户程序的受控ELF子集。
struct elf_header { // ELF64文件头，按规范固定布局。
    uint8_t ident[16]; // 魔数、位宽、字节序、版本和ABI标识。
    uint16_t type, machine; // ET_EXEC与EM_X86_64。
    uint32_t version; // ELF结构版本，当前只接受1。
    uint64_t entry, phoff, shoff; // 用户入口、程序头偏移、节头偏移。
    uint32_t flags; // 目标机器标志，当前要求0。
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; // 各表项长度与数量，装载不读取节表。
};
struct program_header { // ELF64程序头，不是节头。
    uint32_t type, flags; // PT_LOAD及其PF_R/W/X。
    uint64_t offset, address, physical, file_bytes, memory_bytes, align; // 用户装载忽略p_paddr，物理页由内核自己分配。
};
_Static_assert(sizeof(struct elf_header) == 64, "ELF header layout"); // 拒绝宿主结构布局偏离文件格式。
_Static_assert(sizeof(struct program_header) == 56, "ELF program header layout"); // 程序头固定56字节。
static bool fits(uint64_t offset, uint64_t length, size_t bytes) { // 不先做可能回绕的offset+length。
    return offset <= bytes && length <= bytes - offset; // 用减法核对完整输入范围。
}
const char *airi_elf_inspect(const void *image, size_t bytes, struct airi_elf_view *view) { // 先完整校验，再一次性发布结果。
    if (!image || !view || bytes < sizeof(struct elf_header)) // 连最小头都不可读取。
        return "ELF header is missing or truncated"; // 不解引用无效输入。
    struct elf_header h; // 局部对齐对象避免直接把未对齐文件地址强转成结构指针。
    airi_memcpy(&h, image, sizeof(h)); // 只复制已经确认存在的64字节。
    if (h.ident[0] != 0x7f || h.ident[1] != 'E' || h.ident[2] != 'L' || h.ident[3] != 'F' || h.ident[4] != 2 || h.ident[5] != 1 || h.ident[6] != 1) // 固定ELF64、小端、版本1。
        return "ELF identity, class or byte order is unsupported"; // 不猜测其他格式。
    if (h.type != 2 || h.machine != 62 || h.version != 1 || h.flags || h.ehsize != sizeof(h) || h.phentsize != sizeof(struct program_header)) // 固定静态x86-64可执行文件及精确表项大小。
        return "ELF target or header layout is unsupported"; // 动态PIE等目标另行实现。
    if (!h.phnum || h.phnum > 16 || !fits(h.phoff, (uint64_t)h.phnum * h.phentsize, bytes)) // 程序头表也必须完整落在映像内。
        return "ELF program table is invalid"; // 有界数量保证后续乘法不溢出。
```

文件头和完整程序头表现在已可读，下面才逐项解释 LOAD。所有接受结果先写局部 result，文件范围、内存页跨度与权限全部通过后，才在函数末尾一次交给 view；此处仍没有分配目标页。

**继续追加逐段校验与结果发布，闭合 inspect：**

<!-- append: kernel/process/elf.c -->
```c
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
            continue; // 本节不读取节表或辅助元数据内容。
        if (result.count == AIRI_ELF_SEGMENTS || !p.memory_bytes || p.file_bytes > p.memory_bytes || !fits(p.offset, p.file_bytes, bytes)) // 有效数据不能大于内存长度，也不能超出输入映像。
            return "ELF segment file range is invalid"; // 不能从内核映像外复制字节。
        if (p.address < 0x10000 || p.address >= 0x10000000 || p.memory_bytes > 0x10000000 - p.address) // 用户程序限定在低256MiB，避开空页与固定栈区。
            return "ELF segment virtual range is invalid"; // 不允许覆盖内核或栈布局。
        if ((p.flags & ~7u) || !(p.flags & 4u) || (p.flags & 3u) == 3u) // 要求可读，拒绝未知权限和同时可写可执行。
            return "ELF segment permissions are invalid"; // 不默认把所有段设成RWX。
        if ((p.address & 4095) != (p.offset & 4095) || (p.align > 1 && ((p.align & (p.align - 1)) || (p.address % p.align != p.offset % p.align)))) // 页内偏移与段对齐约束必须同时成立。
            return "ELF segment alignment is invalid"; // 不把错位文件字节装到错误地址。
        uint64_t first = p.address & ~4095ull; // 映射必须覆盖段起点所在整页。
        uint64_t end = (p.address + p.memory_bytes + 4095) & ~4095ull; // 范围已经限制到256MiB，加对齐量不会回绕。
        unsigned pages = (unsigned)((end - first) / 4096); // 限制后的跨度可安全转成当前页预算计数。
        if (pages > AIRI_ELF_PAGES - result.pages) // 为初始用户栈保留剩余预算。
            return "ELF exceeds the user page budget"; // 资源量在真正分配前就拒绝。
        for (unsigned j = 0; j < result.count; ++j) { // 不允许不同权限段共享同一物理叶子页。
            const struct airi_elf_segment *old = &result.segments[j]; // 已接受的先前段。
            uint64_t old_first = old->address & ~4095ull; // 以实际映射页范围比较。
            uint64_t old_end = (old->address + old->memory_bytes + 4095) & ~4095ull; // 包含段BSS尾部所在页。
            if (first < old_end && end > old_first) // 即使字节范围不重叠，共享边缘页也会产生权限冲突。
                return "ELF load segments share a page"; // 当前子集明确拒绝这种布局。
        }
        result.segments[result.count++] = (struct airi_elf_segment){p.address, p.offset, p.file_bytes, p.memory_bytes, p.flags}; // 保存验证后的段事实再增加数量。
        result.pages += pages; // 汇总本次真实页预算。
        if ((p.flags & 1u) && h.entry >= p.address && h.entry - p.address < p.file_bytes) // 入口必须落在可执行文件内容，不是只有零填充的尾部。
            entry_found = true; // 所有段结束后仍要检查其他输入是否合法。
    }
    if (!result.count || !entry_found) // 没有LOAD或没有有效入口就不能启动。
        return "ELF entry is outside executable file contents"; // 不靠CPU故障验证基本文件格式。
    *view = result; // 唯一一次修改输出，旧结果在任何失败路径都保持不变。
    return NULL; // 后续装载者可以按已验证范围分配、复制并清BSS。
}
```

主机观察用独立的小端字段写入器构造两段有效输入，故意把p_paddr设为无意义值以核对它不参与装载。每次破坏从有效输入重新开始，分别测试截断、整数边界、目标、权限、重叠、入口、TLS和页预算，要求返回错误且原view逐字节不变。BSS是否真的为零还要让guest中的C程序验证，不能用解析器返回的数字代替实际内存。

**新建 `tests/unit/elf_test.c`。** put按文件小端格式编码，解析器仍独立按固定结构读取。

<!-- write: tests/unit/elf_test.c -->
```c
#include <assert.h> // 作者的正常与损坏ELF观察。
#include <stdio.h> // 宿主只输出结果，不运行用户指令。
#include <airi/elf.h> // 同一段检查算法。
static uint8_t image[0x3000]; // 完整受控输入，不借真实可执行文件的偶然布局。
static void put(unsigned offset, uint64_t value, unsigned bytes) { // 按ELF小端编码原始字段，与解析器结构读取独立。
    for (unsigned i = 0; i < bytes; ++i) // 只写该字段实际宽度。
        image[offset + i] = (uint8_t)(value >> (i * 8)); // 第i个字节取数值的相应低位。
}
static void fixture(void) { // 每个失败只改变一个关键事实。
    airi_memset(image, 0, sizeof(image)); // 清掉上一轮破坏。
    image[0] = 0x7f; // ELF魔数的首字节不是可打印ASCII。
    image[1] = 'E'; // 魔数第二字节。
    image[2] = 'L'; // 魔数第三字节。
    image[3] = 'F'; // 魔数第四字节。
    image[4] = 2; // ELFCLASS64。
    image[5] = 1; // ELFDATA2LSB小端。
    image[6] = 1; // 标识版本1。
    put(16, 2, 2); // e_type=ET_EXEC占两字节。
    put(18, 62, 2); // e_machine=EM_X86_64。
    put(20, 1, 4); // e_version=1占四字节。
    put(24, 0x400000, 8); // e_entry位于第一段的文件内容。
    put(32, 64, 8); // e_phoff紧随文件头。
    put(52, 64, 2); // e_ehsize为64。
    put(54, 56, 2); // e_phentsize为56。
    put(56, 2, 2); // e_phnum只含两个LOAD。
    put(64, 1, 4); // 第一项p_type=PT_LOAD。
    put(68, 5, 4); // 第一项PF_R|PF_X。
    put(72, 0x1000, 8); // 第一段文件字节从4096开始。
    put(80, 0x400000, 8); // 第一段用户虚拟地址。
    put(88, UINT64_MAX, 8); // p_paddr故意无意义，装载必须忽略它。
    put(96, 8, 8); // 第一段文件内容八字节。
    put(104, 4096, 8); // 第一段内存占一页。
    put(112, 4096, 8); // p_align为4096。
    put(120, 1, 4); // 第二项从64+56开始，也是LOAD。
    put(124, 6, 4); // 数据段PF_R|PF_W且没有X。
    put(128, 0x2000, 8); // 数据文件偏移8192。
    put(136, 0x401000, 8); // 数据段紧随代码页，边缘不共享。
    put(152, 4, 8); // 数据段filesz四字节。
    put(160, 8192, 8); // memsz两页，剩余跨度应清零。
    put(168, 4096, 8); // 数据段同样页对齐。
    image[0x1000] = 0xc3; // 代码字节仅作输入，不在宿主执行。
    image[0x2000] = 0x41; // 数据初值不等于零。
}
```

fixture 已形成两段、三页的独立文件输入，数据段只有四字节文件内容，却要求 8192 字节内存。下面的 rejected 保存原 view 的完整字节，corrupt 每次恢复 fixture 后只改一项，main 再逐项要求拒绝与原输出保持。

**继续追加正常与破坏观察：**

<!-- append: tests/unit/elf_test.c -->
```c
static void rejected(size_t bytes) { // 失败不能把局部接受结果写到调用者输出。
    struct airi_elf_view view; // 原输出故意包含可识别旧状态。
    uint8_t before[sizeof(view)]; // 字节快照包含对象表示，用于核对完全未修改。
    airi_memset(&view, 0xa5, sizeof(view)); // 不解引用其中的哨兵指针。
    airi_memcpy(before, &view, sizeof(view)); // 按字节记录真实原状态。
    assert(airi_elf_inspect(image, bytes, &view)); // 必须给出具体静态错误原因。
    assert(!airi_memcmp(before, &view, sizeof(view))); // 不能发布半份解析结果。
}
static void corrupt(unsigned offset, uint64_t value, unsigned bytes) { // 每种破坏都从同一个有效输入重新开始。
    fixture(); // 清掉其他破坏，避免前一次错误掩盖当前分支。
    put(offset, value, bytes); // 只修改这一种字段事实。
    rejected(sizeof(image)); // 要求拒绝并保持旧view不变。
}
int main(void) { // 本程序只验证解析器，不替代guest的BSS和权限运行。
    fixture(); // 两段、三数据页、有效入口。
    struct airi_elf_view view; // 接收完整已验证事实。
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 正常输入应通过。
    assert(view.count == 2 && view.pages == 3 && view.entry == 0x400000); // 页预算与段数量来自实际向外舍入。
    assert(view.segments[1].file_bytes == 4 && view.segments[1].memory_bytes == 8192); // 文件和内存长度明确区分。
    rejected(63); // 不完整文件头。
    corrupt(5, 2, 1); // 大端输入当前不支持。
    corrupt(18, 183, 2); // AArch64不是当前用户CPU目标。
    corrupt(32, sizeof(image)-8, 8); // 程序头表截断。
    corrupt(56, 17, 2); // 程序头数量超过固定界限。
    corrupt(96, 4097, 8); // filesz大于memsz。
    corrupt(72, UINT64_MAX-3, 8); // 文件范围加法回绕不能绕过边界。
    corrupt(80, 0xffffffff80000000ull, 8); // 内核高地址不能作为用户段。
    corrupt(68, 7, 4); // RWX不被本装载策略接受。
    corrupt(112, 3, 8); // 对齐不是2的幂。
    corrupt(136, 0x400000, 8); // 两段共享同一页必须拒绝。
    corrupt(24, 0x401000, 8); // 入口落入可写NX数据段。
    corrupt(64, 3, 4); // PT_INTERP不能被静默忽略。
    corrupt(64, 7, 4); // 未实现TLS也不能当普通静态程序。
    corrupt(104, 64ull*4096, 8); // 程序超过给初始栈预留后的页预算。
    puts("PASS elf: target, bounds, overflow, permissions, overlap, entry, budget, unchanged output"); // 所有失败与正常路径均成立。
    return 0; // 正常退出宿主观察。
}
```

<a id="user-libc-subset"></a>
## 用户库和内核字节库各自链接

装载位置确定后，C 编译产生的调用还需要找到实际函数。hello 使用 memmove，如果把调用目标写成内核的 airi_memcpy 地址，用户取指会被 supervisor 权限拒绝；系统调用允许代办 write，并没有开放任意内核函数。用户库因此单独定义memcpy/memmove/memset/memcmp/strlen，和用户_start一起链接到用户ELF；内核仍链接自己的基础库。两份同名运行库处在不同链接目标和地址空间中，不会因内核包装ELF原始字节而合并符号。

这只是明确列出的libc子集。字节函数采用标准名字，输出和进程接口采用airi前缀，失败直接返回负数，尚未提供errno、完整stdio、malloc、线程或POSIX兼容。memmove按地址方向保护重叠源，memcpy保留不重叠前提；所有指针仍必须指向用户自己的有效范围，库不会替用户越界访问获得特权。

**新建 `labs/airios/include/airi_libc.h`。** 用户编译只读取这一组接口，和include/airi的内核头分开。

<!-- write: labs/airios/include/airi_libc.h -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
#include <stddef.h> // 用户对象字节长度。
#include <stdint.h> // 用户ABI的64位寄存器值。
void *memcpy(void *destination, const void *source, size_t bytes); // 双方不重叠的用户空间复制。
void *memmove(void *destination, const void *source, size_t bytes); // 允许同一用户数组内重叠移动。
void *memset(void *destination, int value, size_t bytes); // 按字节填充用户对象。
int memcmp(const void *left, const void *right, size_t bytes); // 首个不同无符号字节决定结果。
size_t strlen(const char *text); // 用户有效NUL字符串长度。
long airi_write(unsigned fd, const void *buffer, size_t bytes); // 自有int80 ABI，失败直接返回负数。
long airi_getpid(void); // 获取当前用户身份。
_Noreturn void airi_exit(unsigned code); // 退出不返回用户调用方。
long airi_puts(const char *text); // 有界串口write，不自动补换行。
#endif // 结束包含保护。
```

**新建 `labs/airios/libc/bytes.c`。** 完整用户实现，不从内核映像借函数地址。

<!-- write: labs/airios/libc/bytes.c -->
```c
#include <airi_libc.h> // 用户库自己提供标准字节函数，不链接内核实现。
void *memcpy(void *destination, const void *source, size_t bytes) { // 复制完成后返回原目的指针。
    unsigned char *out = destination; // 写方按字节访问。
    const unsigned char *in = source; // 读方不修改内容。
    for (size_t i = 0; i < bytes; ++i) // 零长度不读取任何字节。
        out[i] = in[i]; // 不处理重叠，调用者应使用memmove。
    return destination; // 符合本用户库的标准函数签名。
}
void *memmove(void *destination, const void *source, size_t bytes) { // 重叠时先保护尚未读取的源。
    unsigned char *out = destination; // 目标字节跨度。
    const unsigned char *in = source; // 源字节跨度。
    if ((uintptr_t)out < (uintptr_t)in) // 目标在源前，从低到高不会覆盖未来源。
        for (size_t i = 0; i < bytes; ++i) // 正向移动。
            out[i] = in[i]; // 每次读取之后再覆盖目标。
    else // 目标在源后或相同，从高到低避免覆盖后续源。
        while (bytes) { // 不用无符号i>=0造成下溢死循环。
            --bytes; // 先退到最后一个实际字节。
            out[bytes] = in[bytes]; // 逆序复制仍保留源内容。
        }
    return destination; // 整体完成后返回目的起点。
}
void *memset(void *destination, int value, size_t bytes) { // 填充值只保留低8位。
    unsigned char *out = destination; // 用户可写对象的起点。
    for (size_t i = 0; i < bytes; ++i) // 精确写指定跨度。
        out[i] = (unsigned char)value; // 不把int宽度误当每个元素宽度。
    return destination; // 返回原目的地址。
}
int memcmp(const void *left, const void *right, size_t bytes) { // 只比较调用者声明的有效范围。
    const unsigned char *a = left, *b = right; // 无符号比较避免char符号性改变顺序。
    for (size_t i = 0; i < bytes; ++i) // 相等前缀继续向后。
        if (a[i] != b[i]) // 第一个不同字节就是结果依据。
            return (int)a[i] - (int)b[i]; // 结果正负分别表示左右大小。
    return 0; // 整个跨度相等。
}
size_t strlen(const char *text) { // 有效字符串必须在可访问范围内终止。
    size_t bytes = 0; // 终止零不计入长度。
    while (text[bytes]) // 非零字节继续。
        ++bytes; // 前移到下一个字节。
    return bytes; // 调用方仍负责指针和生命周期。
}
```

字节运算留在用户空间完成，输出则需要越过 K20 的边界。用户调用 airi_write 时，包装函数把普通 C 参数安放到 int80 约定的寄存器，返回时再把内核写进 RAX 的结果交回 C。用户系统调用包装将 RAX 同时作为输入编号和输出结果，`+a`说明同一个C变量既读又写；D、S、d分别指定RDI、RSI、RDX。int80的保存约定沿K20，和Linux syscall会破坏RCX/R11的约定不同。memory/cc阻止编译器假定内存和标志不受这次边界影响。puts在用户空间先strlen，再把指针和长度交给内核完整检查；当前超过256字节会被明确拒绝。

**新建 `labs/airios/libc/syscall.c`。** airi_exit成功不返回，异常返回则用UD2明确失败。

<!-- write: labs/airios/libc/syscall.c -->
```c
#include <airi_libc.h> // 自有用户接口，没有Linux系统调用编号。
static long call3(long number, uint64_t first, uint64_t second, uint64_t third) { // 统一约定RAX/RDI/RSI/RDX。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内核恢复其他通用寄存器，RAX改为结果。
    return number; // +a表示同一个C对象既提供输入又接收输出。
}
long airi_write(unsigned fd, const void *buffer, size_t bytes) { // 用户指针由内核完整检查，库不伪造成功。
    return call3(1, fd, (uintptr_t)buffer, bytes); // 负值直接交给本节调用者，尚无errno变量。
}
long airi_getpid(void) { // 不需要参数的系统调用仍使用同一寄存器入口。
    return call3(3, 0, 0, 0); // 返回当前用户身份。
}
_Noreturn void airi_exit(unsigned code) { // 本函数成功时不返回。
    call3(2, code, 0, 0); // 内核选择启动任务现场继续。
    for (;;) // 若内核错误地返回这里，产生明确用户异常。
        __asm__ volatile("ud2"); // 不在未知退出状态下继续执行。
}
long airi_puts(const char *text) { // 文本长度先在当前用户地址空间计算。
    return airi_write(1, text, strlen(text)); // 当前内核最大256字节，过长会返回-EINVAL。
}
```

<a id="user-c-entry"></a>
## 从用户初始栈进入 main

内核能够恢复到 ELF 的入口，却还不能直接把 main 当作普通 C 函数调用：CPU 进入 ring3 时，没有主机启动库替它准备 argc/argv，也没有一条内核 call 留下用户可用的返回地址。用户 ELF 因此有自己的 _start，负责把初始栈协议转换成 C 调用。这个符号单独链接，不和内核 _start 冲突。进入时RSP指向四个64位槽：argc=1、argv[0]的用户指针、argv结束NULL、空环境NULL；程序名字符串另放在同一用户栈高端。没有辅助向量、动态链接器或完整系统环境，属于本项目自己的最小启动协议。

_start把栈首读入RDI，把紧随的指针数组地址放入RSI，对齐后call main。调用前RSP模16为0，call压入返回地址后C函数入口模16为8。main 的 call 返回地址属于用户 _start。它返回的 EAX 交给 airi_exit，不会返回内核加载器的普通C调用；内核通过K20保留的完整现场继续。

**新建 `labs/airios/start.S`。** xor RBP标明用户C调用链没有上一层栈帧。

<!-- write: labs/airios/start.S -->
```asm
.section .text /* 独立用户ELF的入口，不是内核_start。 */
.global _start /* 用户链接脚本选择此符号。 */
.type _start,@function /* 调试器识别用户入口函数。 */
_start: /* RSP指向内核准备的argc/argv最小启动布局。 */
    xorq %rbp, %rbp /* 初始调用链没有上一层C帧。 */
    movq (%rsp), %rdi /* 第一个C参数argc来自栈首。 */
    leaq 8(%rsp), %rsi /* 第二个参数argv指向紧随的指针数组。 */
    andq $-16, %rsp /* 调用C前保持16字节对齐。 */
    call main /* 运行用户自己链接的普通C函数。 */
    movl %eax, %edi /* main返回值作为exit第一个参数。 */
    call airi_exit /* 用户库通过AiriOS系统调用退出。 */
    ud2 /* _Noreturn函数意外返回时明确失败。 */
.size _start,.-_start /* 记录入口大小。 */
.section .note.GNU-stack,"",@progbits /* 用户栈不要求执行权限。 */
```

**新建 `labs/airios/program.ld`。** 三个权限段从独立整页开始，BSS只占memsz而不要求文件存零。

<!-- write: labs/airios/program.ld -->
```ld
OUTPUT_FORMAT(elf64-x86-64) /* 生成独立静态用户ELF。 */
ENTRY(_start) /* 用户入口由自己的start.S提供。 */
PHDRS { /* 各权限段单独装载，避免共页权限冲突。 */
    text PT_LOAD FLAGS(5); /* 读、执行，不写。 */
    rodata PT_LOAD FLAGS(4); /* 只读常量。 */
    data PT_LOAD FLAGS(6); /* 读写数据和零填充，不执行。 */
}
SECTIONS { /* 固定低地址，当前不支持PIE重定位。 */
    . = 0x400000; /* 与用户装载范围一致的低半区。 */
    .text : { *(.text .text.*) } :text /* 用户入口和函数代码。 */
    . = ALIGN(4096); /* 下一权限段从新页开始。 */
    .rodata : { *(.rodata .rodata.*) } :rodata /* 文字与只读常量。 */
    . = ALIGN(4096); /* 不让常量与可写数据共页。 */
    .data : { *(.data .data.*) } :data /* 文件提供的初始化字节。 */
    .bss : { *(.bss .bss.*) *(COMMON) } :data /* 只增加内存长度，装载器必须清零。 */
    /DISCARD/ : { *(.eh_frame*) *(.note*) } /* 本节不提供异常展开或辅助note消费。 */
}
```

<a id="elf-user-workload"></a>
## 用 C 程序观察装载效果

现在让用户代码消费装载器刚建立的内存。initialized 检查文件初值确实到了可写页，zeroed 检查没有文件内容的尾部确实变成零；仅打印“装载成功”无法证明这两项。initialized 和 zeroed 使用 volatile，要求编译器真正读取装载后的数据；否则已知静态初值可能让检查被优化掉。程序逐字节核对8192字节BSS，写最后一个字节确认尾页可写，再验证自己的栈数组通过五个用户字节函数形成AABCD。argv[0]必须是hello、argv[1]为空，核对的是用户栈中的副本，不是内核指针。

最后的ELF_USER_OK由独立用户ELF中的只读消息经用户puts和系统调用输出；main非零结果会使内核拒绝演示通过。因此文件复制、BSS、权限、入口、参数和用户库真正连成同一次运行。

**新建 `labs/airios/hello.c`。** 此文件是普通用户C程序，不包含内核头。

<!-- write: labs/airios/hello.c -->
```c
#include <airi_libc.h> // 只链接用户库，不引用内核符号。
static volatile unsigned char initialized[4] = {0x41, 0x49, 0x52, 0x49}; // 验证ELF文件提供的可写初始化数据。
static volatile unsigned char zeroed[8192]; // BSS横跨多页，必须由内核按memsz清零。
int main(int argc, char **argv) { // C参数由用户_start从用户初始栈取得。
    if (argc != 1 || !argv || !argv[0] || argv[1] || strlen(argv[0]) != 5 || memcmp(argv[0], "hello", 6)) // 核对最小argc/argv布局和用户字符串。
        return 91; // 用户程序自己报告参数传递失败。
    if (initialized[0] != 0x41 || initialized[3] != 0x49) // .data必须来自ELF文件内容。
        return 92; // 不能把所有段都只清零。
    for (size_t i = 0; i < sizeof(zeroed); ++i) // 核对每个BSS字节，而不是只看首字节。
        if (zeroed[i]) // 页池旧内容不能泄漏进新用户映像。
            return 93; // 不输出成功标记。
    initialized[0] = 0x42; // .data确实可写。
    zeroed[sizeof(zeroed) - 1] = 0x5a; // BSS最后一页也应可写，证明memsz全部映射。
    char bytes[16]; // 用户局部对象位于自己的栈。
    memset(bytes, 0, sizeof(bytes)); // 用户库填充，不调用内核memset。
    memcpy(bytes, "ABCD", 5); // 复制含NUL的四字符字符串。
    memmove(bytes + 1, bytes, 5); // 重叠向后移动，结果为AABCD加NUL。
    if (memcmp(bytes, "AABCD", 6) || zeroed[sizeof(zeroed) - 1] != 0x5a || initialized[0] != 0x42 || airi_getpid() <= 0) // 核对用户库、数据权限和系统调用身份。
        return 94; // 结果不符则main非零退出。
    const char *message = "[ELF_USER_OK] argc=1 data=initialized bss=8192 libc=own\n"; // 该消息来自独立用户ELF的只读段。
    if (airi_puts(message) != (long)strlen(message)) // 真实输出也必须返回完整逻辑长度。
        return 95; // 错误输出不被计作完成。
    return 0; // 用户_start再通过airi_exit把结果交给内核。
}
```

<a id="elf-load-owner"></a>
## 完整准备后再交给 CPU

前面的解析器给出“应装成什么”，这里才把描述变成真正的页。例如 zeroed 所在段先得到清零数据页，再由页表关联用户 VA 与内核借出的 PA；复制文件时只覆盖 filesz，剩余内存保留零。prepare_elf 在分配前校验整个 view，然后先建立内核陷入栈，克隆用户根，对每段按PF_W/PF_X转换成本内核权限，逐页借出并清零。owned数组现在最多64项，页面先登记所有权，映射失败也能由cleanup找到。复制时每次只处理当前页内的文件部分，通过软件lookup得到带偏移的物理地址，再使用HHDM写入尚未运行的页。

所有段完成后另建 RW/NX 初始栈，再调用 `initial_frame(view.entry,"hello")`。hello 含 NUL 共六字节，所以 name_offset=4090，字符串副本位于用户地址 0x70000ffa；四个 64 位槽向下对齐后从页内 4048 开始，初始 RSP 为 0x70000fd0。槽里依次是 1、0x70000ffa、0、0。用户 _start 从第一槽取 argc，把 RSP+8 交作 argv，main 因而沿用户指针读到 hello。内核填写时用的是 HHDM 别名，写进 argv 的数值却必须是用户地址。

这些页与参数都齐备后，initial_frame 才分配 PID、预制返回现场；CPU 尚未执行用户指令。实际切根仍由 int129 统一完成。所有失败在长期根和启动栈中回滚，既不执行半个映像，也不提前释放正在使用的栈。

**局部更新 `include/airi/user.h`。** 接口角色不变，演示现在还包括独立C用户ELF。

依次找到以下旧窗口，各用对应新文本完整替换；其余定义保留。

<!-- replace: include/airi/user.h -->
<!-- before -->
```c
#define AIRI_USER_H // 包含保护。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_demo(void); // 顺序运行正常、非法指令、内核读取和坏栈四个ring3演示并回收。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
void airi_user_tick(void); // 当前单用户上下文的真实时钟计数，尚不调度多个进程。
```
<!-- after -->
```c
#define AIRI_USER_H // 包含保护。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_demo(void); // 运行受控隔离演示与独立C用户ELF，结束后回收。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
void airi_user_tick(void); // 当前单用户上下文的真实时钟计数，尚不调度多个进程。
```

**局部更新 `kernel/process/user.c`。** 原四个隔离程序保留，新增ELF路径和argc/argv布局，正常C程序不要求故意等待时钟。

依次找到以下旧窗口，各用对应新文本完整替换；其余定义保留。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/user.h> // 用户陷入和演示入口。
#include <airi/memory.h> // 物理页、内核区域与长期内核根。
```
<!-- after -->
```c
#include <airi/elf.h> // 对独立静态ELF先校验再装载。
#include <airi/user.h> // 用户陷入和演示入口。
#include <airi/memory.h> // 物理页、内核区域与长期内核根。
```

继续处理下一处：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    struct airi_address_space space; // 自有根与低半区表，借用长期内核映射。
    struct airi_kernel_region stack; // TSS.RSP0指向该内核栈的高端。
    uint64_t owned[2]; // 代码页与用户栈数据页由本对象持有。
    uint64_t fault_error; // 保留用户硬件PF的实际错误位以验证supervisor保护。
    unsigned count, pid, user_ticks; // 数据页数量、用户身份与真实用户态IRQ数。
```
<!-- after -->
```c
    struct airi_address_space space; // 自有根与低半区表，借用长期内核映射。
    struct airi_kernel_region stack; // TSS.RSP0指向该内核栈的高端。
    uint64_t owned[64]; // 有界用户数据页所有权，含ELF各段与初始用户栈。
    uint64_t fault_error; // 保留用户硬件PF的实际错误位以验证supervisor保护。
    unsigned count, pid, user_ticks; // 数据页数量、用户身份与真实用户态IRQ数。
```

继续处理下一处：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    struct airi_trap_frame *frame; // 当前内核栈上完整的用户返回现场。
} process; // 不依赖内核堆扩容，不把它宣称为通用进程表。
static unsigned next_pid = 1; // 每次完整准备后分配新身份，不用槽下标代替 PID。
static struct airi_trap_frame *kernel_return; // int129保存启动任务继续执行所需的完整现场。
```
<!-- after -->
```c
    struct airi_trap_frame *frame; // 当前内核栈上完整的用户返回现场。
} process; // 不依赖内核堆扩容，不把它宣称为通用进程表。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 独立用户ELF的只读输入边界。
static unsigned next_pid = 1; // 每次完整准备后分配新身份，不用槽下标代替 PID。
static struct airi_trap_frame *kernel_return; // int129保存启动任务继续执行所需的完整现场。
```

继续处理下一处：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (!airi_vm_map(&process.space, USER_CODE, process.owned[0], AIRI_VM_USER | AIRI_VM_EXEC) || !airi_vm_map(&process.space, USER_STACK, process.owned[1], AIRI_VM_USER | AIRI_VM_WRITE)) // 用户代码RX，栈RW/NX。
        goto failed; // 页表与数据分别由cleanup回收。
    process.pid = next_pid++; // 准备完成才分配运行身份。
    process.frame = (void *)(uintptr_t)(KERNEL_STACK + 4 * 4096 - sizeof(struct airi_trap_frame)); // 初次iretq现场位于本内核栈顶下方。
    airi_memset(process.frame, 0, sizeof(*process.frame)); // 首次没有旧寄存器值。
    process.frame->rip = USER_CODE; // 处理器将从低半区用户代码取指。
    process.frame->cs = 27; // GDT的ring3代码选择子24加RPL3。
    process.frame->ss = 35; // GDT的ring3数据选择子32加RPL3。
    process.frame->rsp = USER_STACK + 4096 - 16; // 初始用户栈保持16字节对齐。
    process.frame->rflags = 0x202; // 必需bit1加IF，IOPL仍为0，用户不能cli。
    return true; // 还没有切根和进入用户执行。
failed: // 当前仍在长期根和启动栈，半成品都可安全释放。
```
<!-- after -->
```c
    if (!airi_vm_map(&process.space, USER_CODE, process.owned[0], AIRI_VM_USER | AIRI_VM_EXEC) || !airi_vm_map(&process.space, USER_STACK, process.owned[1], AIRI_VM_USER | AIRI_VM_WRITE)) // 用户代码RX，栈RW/NX。
        goto failed; // 页表与数据分别由cleanup回收。
    initial_frame(USER_CODE, NULL); // 保留K20受控汇编布局，用于故障回归。
    return true; // 还没有切根和进入用户执行。
failed: // 当前仍在长期根和启动栈，半成品都可安全释放。
```

继续处理下一处：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return false; // 不运行部分准备的映像。
}
static void run_one(const uint8_t *image, size_t bytes, int expected) { // 同一启动调用链顺序运行四个隔离演示。
    struct airi_memory_stats before, after; // 统计包含页表和两种栈的整个生命周期。
    airi_memory_snapshot(&before); // 准备之前基准。
    if (!prepare(image, bytes)) // 失败已完整回滚。
        airi_panic("cannot prepare user demonstration"); // 不把半成品交给iretq。
    long code; // int129恢复到启动现场时由RAX交回结束原因。
    __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // DPL0门保存当前完整现场，再切到受控用户现场。
    if (code != expected || (!expected && process.user_ticks < 2) || (expected == -142 && process.fault_error != 5)) // 正常程序必须真实运行并被用户态IRQ打断。
        airi_panic("user context ended with unexpected result"); // 用户GP与正常退出分别验证。
    cleanup(); // 已经回启动栈，才允许归还用户内核栈。
```
<!-- after -->
```c
    return false; // 不运行部分准备的映像。
}
static void run_one(const uint8_t *image, size_t bytes, int expected, bool elf, bool require_ticks) { // 同一启动调用链顺序运行隔离演示与独立用户ELF。
    struct airi_memory_stats before, after; // 统计包含页表和两种栈的整个生命周期。
    airi_memory_snapshot(&before); // 准备之前基准。
    bool prepared = elf ? prepare_elf(image, bytes) : prepare(image, bytes); // 两种输入通过各自明确装载边界。
    if (!prepared) // 失败已完整回滚。
        airi_panic("cannot prepare user demonstration"); // 不把半成品交给iretq。
    long code; // int129恢复到启动现场时由RAX交回结束原因。
    __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // DPL0门保存当前完整现场，再切到受控用户现场。
    if (code != expected || (require_ticks && process.user_ticks < 2) || (expected == -142 && process.fault_error != 5)) // 正常程序必须真实运行并被用户态IRQ打断。
        airi_panic("user context ended with unexpected result"); // 用户GP与正常退出分别验证。
    cleanup(); // 已经回启动栈，才允许归还用户内核栈。
```

继续处理下一处：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
}
void airi_user_demo(void) { // 当前只验证边界，不提供shell启动和完整进程调度。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), 0); // 用户输出、非法指针和真实IRQ后正常退出。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), -141); // CPL3执行cli得到GP13，仅结束该用户。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), -142); // 真实user读内核PTE得到PF error=5。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), -22); // 在内核iretq之前拒绝不规范RSP。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 内核恢复后继续串口，不宣称ELF/fork已经完成。
}
```
<!-- after -->
```c
}
void airi_user_demo(void) { // 当前只验证边界，不提供shell启动和完整进程调度。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), 0, false, true); // 用户输出、非法指针和真实IRQ后正常退出。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), -141, false, false); // CPL3执行cli得到GP13，仅结束该用户。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), -142, false, false); // 真实user读内核PTE得到PF error=5。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), -22, false, false); // 在内核iretq之前拒绝不规范RSP。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), 0, true, false); // 真正解析静态ELF、构造用户栈并运行C与自有libc。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 内核恢复后继续串口，此记录只覆盖受控用户边界；ELF 由独立装载演示核对，fork 尚未实现。
}
```

继续处理下一处：

已有调用点现在分别交出入口与程序名。先在原 prepare 定义前加入 initial_frame：按前面的 hello 栈布局写好用户副本，最后预制首次现场。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool prepare(const uint8_t *image, size_t bytes) { // 本节是受控位置无关字节映像，下一节再解析独立ELF。
```
<!-- after -->
```c
static void initial_frame(uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
    if (name) { // C程序使用本项目最小argc/argv栈协议。
        size_t bytes = airi_strlen(name) + 1; // 包含用户字符串的终止零。
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&process.space, USER_STACK, &physical, NULL)) // 栈必须已建立。
            airi_panic("initial user stack mapping is missing"); // 不猜测owned数组中的位置。
        uint8_t *alias = airi_memory_direct(physical); // 通过内核HHDM填写尚未运行的用户栈。
        size_t name_offset = 4096 - bytes; // 字符串放在栈页高端。
        airi_memcpy(alias + name_offset, name, bytes); // 用户只得到自己的字符串副本。
        size_t words_offset = (name_offset & ~(size_t)15) - 4 * sizeof(uint64_t); // 四个64位槽，入口RSP仍16字节对齐。
        uint64_t *words = (uint64_t *)(alias + words_offset); // 已对齐的栈内数组。
        words[0] = 1; // argc目前固定一个程序名。
        words[1] = USER_STACK + name_offset; // argv[0]必须是用户VA，不是HHDM指针。
        words[2] = 0; // argv[1]结束指针数组。
        words[3] = 0; // 空环境，当前没有完整ELF辅助向量。
        stack_pointer = USER_STACK + words_offset; // CPU初始RSP指向argc槽。
    }
    process.pid = next_pid++; // 资源和布局完成后才取得身份。
    process.frame = (void *)(uintptr_t)(KERNEL_STACK + 4 * 4096 - sizeof(struct airi_trap_frame)); // 内核栈顶部预制首次iretq现场。
    airi_memset(process.frame, 0, sizeof(*process.frame)); // 首次没有旧GPR。
    process.frame->rip = entry; // ELF入口或已知汇编入口。
    process.frame->cs = 27; // 用户代码段与RPL3。
    process.frame->ss = 35; // 用户数据段与RPL3。
    process.frame->rsp = stack_pointer; // 使用刚准备的用户布局。
    process.frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool prepare(const uint8_t *image, size_t bytes) { // 这里只准备受控位置无关字节映像；独立 ELF 由 prepare_elf 装载。
```

继续处理下一处：

初始布局的助手已有了，逐页所有权由 owned_page 统一登记。它先记录并清零物理页，再映射；映射失败时 cleanup 仍能找到这次已借出的页。继续在原 prepare 前插入此函数。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool prepare(const uint8_t *image, size_t bytes) { // 这里只准备受控位置无关字节映像；独立 ELF 由 prepare_elf 装载。
```
<!-- after -->
```c
static bool owned_page(uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (process.count == 64) // 包括初始栈在内的全部数据页预算。
        return false; // 不越过所有权数组。
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) // 耗尽时没有需要登记的页。
        return false; // 调用方回滚已有部分。
    process.owned[process.count++] = physical; // 先登记，映射失败也不会泄漏本次物理页。
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    return airi_vm_map(&process.space, address, physical, flags | AIRI_VM_USER); // 所有低半区叶子必须明确USER。
}
static bool prepare(const uint8_t *image, size_t bytes) { // 这里只准备受控位置无关字节映像；独立 ELF 由 prepare_elf 装载。
```

继续处理下一处：

两个助手写齐以后，prepare_elf 才能组合整份装载：inspect 先给 view，逐段建内存与复制，独立栈完成后调用 initial_frame。继续在原 prepare 前插入；原四个受控映像仍使用保留的 prepare。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool prepare(const uint8_t *image, size_t bytes) { // 这里只准备受控位置无关字节映像；独立 ELF 由 prepare_elf 装载。
```
<!-- after -->
```c
static bool prepare_elf(const uint8_t *image, size_t bytes) { // 当前还没有exec，先在空上下文中完成事务装载。
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return false; // 旧输出和当前空上下文都未改变。
    }
    if (!airi_memory_region_create(&process.stack, KERNEL_STACK, 4)) // 内核陷入栈先完成，再借高半区。
        return false; // 区域接口已回滚失败。
    if (!airi_memory_user_space(&process.space)) // 创建独立低半区。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
        unsigned flags = ((segment->flags & 2) ? AIRI_VM_WRITE : 0) | ((segment->flags & 1) ? AIRI_VM_EXEC : 0); // ELF PF标志转换成本内核权限枚举。
        uint64_t first = segment->address & ~4095ull; // 覆盖段首所在页。
        uint64_t end = (segment->address + segment->memory_bytes + 4095) & ~4095ull; // 包含完整BSS跨度。
        for (uint64_t address = first; address < end; address += 4096) // 所有页面先清零再接入。
            if (!owned_page(address, flags)) // 物理耗尽或映射失败都终止本次装载。
                goto failed; // 清理根/表后再归还数据页。
        for (uint64_t done = 0; done < segment->file_bytes;) { // 只复制filesz字节，memsz剩余继续保持零。
            uint64_t physical; // lookup结果包含当前页内偏移。
            if (!airi_vm_lookup(&process.space, segment->address + done, &physical, NULL)) // 已映射跨度必须可查询。
                goto failed; // 内部装载状态不一致也不运行。
            uint64_t part = 4096 - ((segment->address + done) & 4095); // 当前页内可复制长度。
            if (part > segment->file_bytes - done) // 不从文件右端外复制字节。
                part = segment->file_bytes - done; // 最后一次缩到实际剩余。
            airi_memcpy(airi_memory_direct(physical), view.image + segment->offset + done, (size_t)part); // 内核控制的物理别名可初始化最终只读/可执行页。
            done += part; // 下一次继续剩余文件内容。
        }
    }
    if (!owned_page(USER_STACK, AIRI_VM_WRITE)) // 栈单独RW/NX，不采用ELF可执行栈请求。
        goto failed; // 失败仍统一回滚。
    initial_frame(view.entry, "hello"); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, process.count); // 不含页表与内核栈的用户数据页数量。
    return true; // 还未切CR3，运行入口统一由int129交接。
failed: // 长期根仍活动，用户空间从未开始执行。
    cleanup(); // 页表、用户数据和内核栈全部回收。
    return false; // 不提供部分映像。
}
static bool prepare(const uint8_t *image, size_t bytes) { // 这里只准备受控位置无关字节映像；独立 ELF 由 prepare_elf 装载。
```

<a id="independent-user-build"></a>
## 两次链接，再包装原始输入

装载器的输入到底从哪里来？本章还没有文件系统，先在构建时把完整 ELF 当作数据嵌进内核，运行时再解析它。这个安排包含两次不同的链接，不能把用户对象直接混进内核链接。Make 先用用户 small 地址模型分别编译_start、hello和用户库，使用用户program.ld链接hello.elf。它不使用内核高地址脚本，也不链接宿主启动文件或libc。第二步的images.S只用.incbin包装当前配置的完整ELF字节；这个包装对象才进入内核链接，用户_start等符号仍只在用户ELF内部。

images.o显式依赖同一OUT下的hello.elf，防止用户程序变化后仍嵌入旧文件。AIRI_HELLO_ELF由Make作为带引号的路径宏传入；它是构建时的输入路径，不是guest会打开的宿主文件。运行时内核只读取已嵌入的受界限保护字节，后续文件系统阶段再提供其他映像来源。

**新建 `labs/airios/images.S`。** 它不执行ELF，也不把用户对象直接链接到内核符号表。

<!-- write: labs/airios/images.S -->
```asm
.section .rodata.user_elf,"a" /* 原始ELF作为内核只读输入，不在内核直接执行。 */
.balign 8 /* 输入开头至少按8字节对齐，解析仍使用memcpy。 */
.global airi_hello_elf_start, airi_hello_elf_end /* 编译期固定映像边界。 */
airi_hello_elf_start: /* 文件头也包含在输入字节中。 */
.incbin AIRI_HELLO_ELF /* Make传入当前配置自己的ELF路径，不复制另一配置旧产物。 */
airi_hello_elf_end: /* 解析器只允许读取边界之内。 */
.section .note.GNU-stack,"",@progbits /* 包装对象不需要可执行栈。 */
```

**局部更新 `Makefile`。** 新增用户构建规则和同一解析器的ASan/UBSan观察。

依次找到以下旧窗口，各用对应新文本完整替换；其余定义保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续处理下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```
<!-- after -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```

继续处理下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```
<!-- after -->
```make
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 解析器在独立小端字节夹具中核对正常与损坏ELF，不执行输入指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/elf_test.c \
	    kernel/process/elf.c kernel/lib/base.c -o build/tests/elf_test
# 失败不得写出半份view，也不得越界读取映像。
	./build/tests/elf_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```

继续处理下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info
```
<!-- after -->
```make
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户独立静态ELF是嵌入输入，不是内核的直接链接对象。
$(OUT)/user/hello.elf: $(USER_OBJECTS) labs/airios/program.ld
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -c $< -o $@
```

**局部更新 `scripts/boot.py`。** 正常完成还必须看到ELF_LOAD与真正的ELF_USER_OK。

依次找到以下旧窗口，各用对应新文本完整替换；其余定义保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
```
<!-- after -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
```

<!-- run -->
```sh
./airios test  # 五组主机边界、六种启动、三条真实输入都运行本节镜像。
PYTHONPATH=scripts python3 - <<'PYELF'  # 在构建主机只读检查独立用户产物。
import subprocess  # 参数列表启动ELF工具。
from tools import ROOT, tool  # 已有工具选择与当前根目录。
binary = ROOT / 'build/normal/user/hello.elf'  # 当前配置的独立用户文件。
report = subprocess.check_output([tool('llvm-readelf', 'llvm'), '-h', '-l', '-d', str(binary)], text=True)  # 打印文件头、LOAD权限和动态信息。
print(report)  # 观察filesz/memsz的实际差别。
assert 'INTERP' not in report and '(NEEDED)' not in report  # 不能依赖宿主解释器或动态库。
assert not subprocess.check_output([tool('llvm-nm', 'llvm'), '-u', str(binary)]).strip()  # 用户符号全部由自有对象解析。
PYELF
```

既有 QEMU 记录输出`[ELF_LOAD] entry=0x400000 segments=3 data_pages=6 argc=1`，然后`[ELF_USER_OK] argc=1 data=initialized bss=8192 libc=own`，pid5正常退出0，页池与堆空闲恢复。实际.data/BSS段包含对齐空隙，所以memsz和映射页数不只等于8192数组本身；以当前readelf报告为准。主机ELF检查覆盖正常输入、十四类破坏和原view保持；宿主不执行这些用户指令。

故障操作已完整写在elf_test.c：每次只改一个字段，错误字符串指出格式或范围，旧view保持原字节。这里还没有exec替换运行中的地址空间；错误ELF与旧进程的事务交接会在exec接入时一并验证，不能提前宣称已完成。

调试沿L01的QEMU/GDB连接，在airi_elf_inspect断点处等待前四个受控演示结束后，再给GDB加入独立用户符号：`add-symbol-file hello.elf 0x400000`、`hbreak main`、`continue`。hello.elf要先用scp传到自己的调试目录；内核批量调试脚本尚未自动发送这个新产物。main处查看argc、argv[0]、zeroed和initialized，再对照当前CR3和用户CS。此前远端 SSH 连接失败，交互 GDB 停点仍未验证。

下一步让多个用户上下文并存，时钟在完整用户现场边界轮转，再以页内容直接复制实现首版fork，使用本节ELF装载为exec准备新空间，wait负责结束状态和回收。COW、动态链接、完整POSIX和多线程进程继续保持明确边界。

依据：[System V gABI程序头](https://doc.sco.com/developers/gabi/latest/ch5.pheader.html)的LOAD、filesz/memsz与对齐定义，以及[x86-64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI)的函数调用约定。我们的初始栈省略完整辅助向量，不宣称可直接运行普通Linux发行版程序。
