# K51 · TLS 的零填充尾部不必占普通 LOAD 的空间

从 K50 继续。静态TLS已经能为用户程序建立本地变量，新的映像却可能在进入main之前被拒绝：它有4字节TLS初值、较长的TLS零尾部，普通BSS只有1字节。这里用ctls-tail单独复现这类布局，修正把整个TLS长度都要求落入普通LOAD的过严检查。线程运行库仍不是本节前置。

<a id="tls-tail-model"></a>
## 文件初值与每个执行者需要的总空间

PT_LOAD描述程序普通地址空间中的可装载范围，filesz是文件提供的字节，memsz还包含普通零填充。PT_TLS描述另一份初始化模板：filesz给出本地变量的初值字节，memsz给出每个执行者需要的本地空间。内核从文件复制filesz字节到独立TLS运行块，其余清零；该运行块位于0x60000000附近，不以模板的原VA直接作为当前变量地址。

TLS的.tbss是没有文件字节的零尾部。链接器可以让它不增加普通LOAD的内存长度；后面的普通.bss也可能使用相同附近的模板VA，而不为.tbss额外保留跨度。这是模板与运行副本分开的结果。这种TLS NOBITS行为见[LLVM原始实现说明](https://reviews.llvm.org/D13838)。

例如本轮ctls-tail由LLD产生的TLS filesz=4、memsz=273，而普通RW LOAD filesz=4、memsz=5。257个零字节数组之前还有对齐填充，所以TLS总长不是简单的4+257。旧检查要求273不超过普通LOAD的5，因而拒绝；实际需要由LOAD提供的是4字节初值，273字节运行副本仍按原布局另外申请并清零。

<a id="tls-tail-program"></a>
## 用小普通 BSS 配合较长的本地零数组

新建ctls-tail.c。initial有文件初值7，zeros[257]只有TLS内存需求，ordinary则是普通BSS的一字节。main分别读取、修改两个TLS边界及普通字节，确认它们来自独立的运行存储。volatile使编译器真正执行这些用户读写，不能只用常量判断替代运行副本。

<!-- write: labs/airios/ctls-tail.c -->
```c
#include <airi_libc.h> // 单执行者观察TLS零尾部，不依赖线程库或errno。
static _Thread_local volatile unsigned initial = 7; // 四个文件初值字节，位于tdata。
static _Thread_local volatile unsigned char zeros[257]; // 较长的tbss，运行副本需要清零但普通LOAD不为它预留全部范围。
static volatile unsigned char ordinary; // 普通BSS只有一字节，明显短于TLS零尾部。
int main(void) { // 装载器应接受真实LLD的独立初值与零尾部长度。
    if (initial != 7 || ordinary) // TLS初值和普通LOAD零字节各自正确。
        return 51; // 不把两种存储误作同一个对象。
    for (unsigned i = 0; i < sizeof(zeros); ++i) // 全部运行TLS零尾部都由单独副本提供。
        if (zeros[i]) // 普通LOAD的memsz短不能导致TLS运行块遗漏清零。
            return 52; // 任何旧内容都属于装载错误。
    zeros[0] = 0x31; zeros[256] = 0x72; ordinary = 0x53; // 分别写两个TLS边界和普通BSS。
    if (initial != 7 || zeros[0] != 0x31 || zeros[256] != 0x72 || ordinary != 0x53) // 相似模板VA不代表运行对象互相覆盖。
        return 53; // 普通BSS不能别名到真正TLS运行副本。
    airi_puts("[CTLS_TAIL_OK] initialized=4 zero_tail=257 ordinary_bss=1 independent=1\n"); // 所有真实用户读写完成后发布。
    return 0; // shell继续，用户内存仍按既有退出路径整体归还。
}
```

<a id="tls-tail-validation"></a>
## 仍检查完整初值，零尾部由运行块提供

elf.c对单个PT_TLS的大小、对齐、文件范围和用户VA检查保持。程序头全部读取后，继续查找一个合法LOAD作为模板起点的依据：delta为TLS模板VA减LOAD起点，必须不超过该段的普通内存长度。纯零模板没有需要读取的初值，也允许锚定在段的右端；完全脱离所有LOAD的模板仍被当前子集拒绝。

有非零filesz时，delta和这份初值必须完整落在LOAD的文件跨度内，PT_TLS的文件偏移还必须等于LOAD偏移加delta。这样不能把任意可读文件字节伪装成模板，也不能因为允许较长.tbss而放过缺失的初值。唯一撤掉的是“整个TLS memsz都必须被普通LOAD memsz包住”的额外条件。

运行副本继续根据TLS memsz及TCB计算页数，计入用户页预算；没有减少实际分配或把零尾部留成未映射。解析仍先写局部result，所有失败不修改调用者旧view。更新elf.c，其他ELF规则保持。

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
    if (result.tls.memory_bytes) { // 所有LOAD都已读完，TLS头可以出现在它们之前。
        bool contained = false; // 文件初值必须由一个LOAD提供，tbss零尾部不占普通LOAD内存长度。
        for (unsigned i = 0; i < result.count; ++i) { // 最多八项，逐个核对对应关系。
            const struct airi_elf_segment *segment = &result.segments[i]; // 已验证用户范围与文件边界。
            if (result.tls.address < segment->address) // 用减法前先确认有非负偏移。
                continue; // 不能将负值回绕成小范围。
            uint64_t delta = result.tls.address - segment->address; // TLS模板在该段中的相对位置。
            if (delta > segment->memory_bytes) // 非空初值锚定段内，纯零模板也允许锚定段的右端。
                continue; // 仍不接受完全脱离LOAD的模板起点。
            if (result.tls.file_bytes && (delta > segment->file_bytes || result.tls.file_bytes > segment->file_bytes - delta || result.tls.offset != segment->offset + delta)) // 有初值时VA和文件位置必须对应同一LOAD字节。
                continue; // 纯零尾部不读取文件字节，也不要求普通LOAD为它保留全部VA跨度。
            contained = true; // 初值完整且起点合法，运行副本仍单独分配memory_bytes。
        }
        if (!contained || result.tls.pages > AIRI_ELF_PAGES - result.pages) // 运行副本也占用户预算，不能因模板已计页就漏算复制页。
            return "ELF TLS is outside LOAD or exceeds the user page budget"; // 真正分配前拒绝，旧view不变。
    }
    *view = result; // 唯一一次修改输出，旧结果在任何失败路径都保持不变。
    return NULL; // 后续装载者可以按已验证范围分配、复制并清BSS。
}
```

<a id="tls-tail-faults"></a>
## 合法零尾部与缺失初值分开观察

host的tls_cases先保留原73字节模板，再把普通LOAD memsz改为4，四字节初值仍完整，应该通过。接着把LOAD的filesz和memsz都缩为1，LOAD本身仍合法，但不足以提供TLS的四字节初值，必须拒绝并保持旧view。两种输入只差实际可提供的文件内容，不能统一按TLS总长拒绝。

再构造纯零模板，把起点放在四字节LOAD的右端，解析通过且不读取该位置；将起点移到完全无LOAD的0x500000，则继续拒绝。最后恢复fixture，让已有超限、错误对齐、重复TLS、页预算和程序头顺序检查独立运行。用下面完整终点更新elf_test.c。

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
    put(160, 4, 8); // 普通LOAD只含四字节tdata，TLS零尾部不要求占同样的虚拟范围。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && view.pages == 2 && view.tls.memory_bytes == 73); // 对应真实LLD的短普通BSS与较长tbss组合。
    put(152, 1, 8); put(160, 1, 8); // LOAD自身合法但只提供一字节，不能提供四字节TLS初值。
    rejected(sizeof(image)); // 放开零尾部不能放过缺失的文件初值。
    tls_fixture(); put(160, 4, 8); put(208, 0, 8); put(192, 0x401004, 8); put(224, 1, 8); // 纯零模板可以从LOAD右端开始，无需读取那个位置。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && !view.tls.file_bytes); // 运行副本的73字节仍单独清零。
    put(192, 0x500000, 8); // 完全离开LOAD范围仍是本子集不接受的孤立模板。
    rejected(sizeof(image)); // 零初值不能跳过锚点与受控地址检查。
    tls_fixture(); // 恢复原合法模板，再观察后续小对齐和极值路径。
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

<a id="tls-tail-integration"></a>
## 接入同一份初始程序目录

Makefile末尾新增ctls-tail.elf，复用原始USER_COMMON，不链接posix.o或线程库。images.S以只读incbin包入实际ELF；user.c只增加范围声明与ctls-tail目录项。进程、执行者、FS恢复和用户TLS初始化代码保持K50状态。

shell_test在ctls完成以后执行ctls-tail，只有完整独立标记与实际退出0都出现才继续。新增名字后的完整目录仍容得下原256字节预算，不需要改变shell。下面给出这几份文件的完整终点，已有其余内容保留。

<details>
<summary>Makefile 的完整终点</summary>

<!-- write: Makefile -->
```make
# 用既有make_command取得交叉工具参数；统一airios入口在K13加入。
# 内核C编译器由调用参数覆盖；此处默认clang，不借宿主libc。
CC := clang
# ELF链接器默认LLD，调用参数可传入真实找到的路径。
LD := ld.lld
# 主机实验用宿主编译器，可以链接宿主libc。
HOST_CC := cc
# ?=仅在调用者未赋值时取normal，各测试配置独立构建。
PROFILE ?= normal
# $(PROFILE)展开当前配置名称，OUT指向这一配置的产物目录。
OUT := build/$(PROFILE)
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/fpu.c kernel/arch/x86_64/tls.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
# guest 图形适配与纯 C 前端进入同一内核，参考服务和 Cocoa 不进入。
SOURCES += kernel/console/display.c $(UI_SOURCES)
else # 没有desktop-前缀时只选择串口实现。
# 串口实现没有字体、绘图与桌面状态。
SOURCES += kernel/console/serial_display.c
endif # 结束当前可选图形分支。
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/fpu_seed.o $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 预处理器从自有include、固定协议头third_party和前端include取得声明。
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -DAIRI_GRAPHICAL_CONSOLE -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifneq ($(filter no-framebuffer service-no-framebuffer,$(CASE)),)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 服务目标只改变启动策略，与旧九种诊断目标使用独立产物目录。
ifneq ($(filter service service-no-framebuffer service-init-failure,$(CASE)),)
CPPFLAGS += -DAIRI_BOOT_SERVICE # 初始化完成后直接进入用户init，不运行教学演示。
endif # 普通配置继续原诊断路径。
ifeq ($(CASE),service-init-failure)
CPPFLAGS += -DAIRI_TEST_INIT_FAILURE # 只在此故障镜像把初始程序换成截断ELF。
endif # 不修改正常init或用户资源接口。
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 设备超时故障镜像故意不发布avail.idx，正常镜像不含注入。
ifeq ($(CASE),block-timeout)
CPPFLAGS += -DAIRI_TEST_BLOCK_TIMEOUT # 验证真实PIT截止与DMA隔离，不冒充设备断电。
endif # 结束块设备故障配置。
# 设备完成存在但故意不交付IRQ，验证PIT驱动的used环后备收集。
ifeq ($(CASE),block-irq-masked)
CPPFLAGS += -DAIRI_TEST_BLOCK_IRQ_MASKED # 不伪造完成，仍使用真实DMA与used.idx。
endif # 其他配置正常开放PCI设备IRQ。
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
endif # 结束栈故障配置。
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
CPPFLAGS += -DAIRI_TEST_TIMER_MASKED # PIT仍计数，但PIC阻止IRQ0到达CPU。
endif # 结束时钟故障条件。
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
CPPFLAGS += -DAIRI_TEST_FATAL_TRAP # 仅此构建目录触发未处理 UD2。
endif # 结束故障配置。
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
# 单引号由shell保护内部双引号，编译器取得C字符串宏值；下一续行同时设置学习日志文字。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束 edit-experiment 的文字覆盖条件

# 这些名称是动作目标，即使同名文件存在也执行其规则。
.PHONY: all kernel image host-test clean
# 默认动作依赖镜像，触发内核编译、链接和镜像制作。
all: image
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c Makefile
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S Makefile
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF依赖全部对象与链接脚本，任一变化会重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
# 这一完整续行命令用ELF x86-64、无标准库、静态链接、4KiB页对齐和不可执行栈；-T指定自有布局，最后-o写目标。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# 镜像先要求独立ELF成功。
image: $(OUT)/airios.elf
# 把同一配置的ELF装入UEFI启动镜像。
	python3 scripts/image.py $(PROFILE)
# 主机字节算法、启动合同与命令状态检查，保留assert并启用ASan/UBSan。
host-test:
# 先建立主机检查产物目录。
	@mkdir -p build/tests
# 以下完整续行命令编译主机检查；fno-builtin避免优化绕过被测实现，sanitizer检查内存和未定义行为，AIRI_HOST_TEST避开真实端口指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/base_test.c \
	    kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c kernel/arch/x86_64/input.c \
		    -o build/tests/base_test
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 独立编译同一物理页算法，主机不解引用 guest 地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/pages_test.c \
	    kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/pages_test
# 验证耗尽、保留范围、重复释放和舍入边界。
	./build/tests/pages_test
# 同一四级页表算法在受控arena里验证，绝不安装宿主页表。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/vm_test.c \
	    kernel/arch/x86_64/paging.c kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/vm_test
# 覆盖翻译、共享分支、空表回收和中途耗尽的所有权回滚。
	./build/tests/vm_test
# 在宿主受控backing上验证同一小对象实现，不借用guest地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/heap_test.c \
	    kernel/mm/heap.c kernel/lib/base.c -o build/tests/heap_test
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 解析器在独立小端字节夹具中核对正常与损坏ELF，不执行输入指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/elf_test.c \
	    kernel/process/elf.c kernel/lib/base.c -o build/tests/elf_test
# 失败不得写出半份view，也不得越界读取映像。
	./build/tests/elf_test
# 文件算法在主机使用同一实现，验证引用、偏移和容量回滚。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/file_test.c \
	    kernel/fs/file.c kernel/lib/base.c -o build/tests/file_test
# 仅全部文件边界成立才继续启动检查。
	./build/tests/file_test
# 日志后端模型验证提交顺序、错误返回与掉电缓存边界。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/journal_test.c \
	    kernel/storage/journal.c kernel/lib/base.c -o build/tests/journal_test
# 模型不声称真实主机掉电，真设备另由guest演示。
	./build/tests/journal_test
# 异步队列用明确延后后端验证SQ/CQ、代数句柄和晚完成。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/async_test.c \
	    kernel/io/engine.c kernel/lib/base.c -o build/tests/async_test
# 全部模型边界通过后才继续真实guest。
	./build/tests/async_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
	python3 -c 'import shutil; shutil.rmtree("build", ignore_errors=True)'
# 后缀替换得到自动依赖文件；前缀-允许首次构建尚无.d。
-include $(OBJECTS:.o=.d)


# 手动选择图形动作时才读取支线目标；默认构建不检查该目录。
ifneq ($(filter ui-preview ui-test lab-editor,$(MAKECMDGOALS)),)
include frontend/targets.mk # 仅显式图形动作读取相应规则。
endif # 结束当前可选图形分支。

# 三个观察名称均是动作，即使有同名文件也运行。
.PHONY: lab-memmove lab-boot-info
# 重新构建重叠移动与截断输出观察。
lab-memmove:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/base/memmove_trace.c kernel/lib/base.c -o build/labs/memmove
# 打印左/右重叠及截断结果。
	./build/labs/memmove
# 重新构建合法/损坏启动数据观察。
lab-boot-info:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/boot/boot_info_probe.c kernel/boot/info.c -o build/labs/boot-info
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -mno-avx -ftls-model=local-exec -MMD -MP -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S Makefile
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户独立静态ELF是嵌入输入，不是内核的直接链接对象。
$(OUT)/user/hello.elf: $(USER_OBJECTS) labs/airios/program.ld
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf $(OUT)/user/waits.elf $(OUT)/user/async.elf Makefile
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -DAIRI_STORAGE_ELF='"$(OUT)/user/storage.elf"' -DAIRI_PIPES_ELF='"$(OUT)/user/pipes.elf"' -DAIRI_WAITS_ELF='"$(OUT)/user/waits.elf"' -DAIRI_ASYNC_ELF='"$(OUT)/user/async.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 多进程工作负载有自己的独立静态ELF。
$(OUT)/user/processes.elf: $(USER_COMMON) $(OUT)/user/processes.o labs/airios/program.ld
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# worker也有自己的_start和main，原始ELF只在包装时进入内核。
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 只链接这份工作负载的main。
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# storage与recover由同一ELF按明确argv分支。
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 管道程序只复用自有用户入口与库。
$(OUT)/user/pipes.elf: $(USER_COMMON) $(OUT)/user/pipes.o labs/airios/program.ld
# 保持相同低地址ELF布局。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# ELF只选择waits的main。
$(OUT)/user/waits.elf: $(USER_COMMON) $(OUT)/user/waits.o labs/airios/program.ld
# 低地址静态链接，不借宿主入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@

# 异步用户程序与重置别名共用独立main和低地址入口。
$(OUT)/user/async.o: labs/airios/async.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 当前配置的对象目录。
	@mkdir -p $(dir $@)
# USER_FLAGS含公开ABI头，不能依赖内核内部结构。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 自有入口、库与async main单独链接。
$(OUT)/user/async.elf: $(USER_COMMON) $(OUT)/user/async.o labs/airios/program.ld
# 仍为无宿主libc的静态ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@

# shell与贯通演示是独立的低地址用户ELF，不链接彼此main。
$(OUT)/user/%.o: labs/airios/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 与既有libc模式规则路径不同，Make按存在的源选择。
	@mkdir -p $(dir $@)
# 复用整数用户ABI，不借宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 两个新程序各自链接自有入口与库。
$(OUT)/user/sh.elf: $(USER_COMMON) $(OUT)/user/sh.o labs/airios/program.ld
# 每个目标包含且只包含自己的main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/sh.o -o $@
# demo同样形成真正ELF，再作为初始装载输入。
$(OUT)/user/demo.elf: $(USER_COMMON) $(OUT)/user/demo.o labs/airios/program.ld
# 段权限与BSS由既有用户链接脚本定义。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/demo.o -o $@
# 目标额外依赖保证.incbin永远读取本配置已完成ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/sh.elf $(OUT)/user/demo.elf
# target-specific变量只对这个包装对象追加路径宏，单引号保护C字符串。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_SHELL_ELF='"$(OUT)/user/sh.elf"' -DAIRI_DEMO_ELF='"$(OUT)/user/demo.elf"'

# 孤儿工作负载复用用户入口和库，保持独立main。
$(OUT)/user/orphans.elf: $(USER_COMMON) $(OUT)/user/orphans.o labs/airios/program.ld
# 独立静态ELF不链接宿主libc。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/orphans.o -o $@
# 包装必须等待此配置自己的ELF完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/orphans.elf
# 汇编只读当前文件路径，不能复用另一配置产物。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_ORPHANS_ELF='"$(OUT)/user/orphans.elf"'

# 动态页工作负载与其他用户main分别链接。
$(OUT)/user/memory.elf: $(USER_COMMON) $(OUT)/user/memory.o labs/airios/program.ld
# 静态ELF只使用自有用户入口与库。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/memory.o -o $@
# 当前包装对象要等本配置的新映像完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/memory.elf
# 文件名作为C字符串宏传入预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MEMORY_ELF='"$(OUT)/user/memory.elf"'

# init与shell拥有各自用户main，监督策略在ring3运行。
$(OUT)/user/init.elf: $(USER_COMMON) $(OUT)/user/init.o labs/airios/program.ld
# 仍使用自有入口和整数用户库，尚未引入POSIX运行时。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/init.o -o $@
# 用户目录包装等本配置init完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/init.elf
# init文件路径作为明确的预处理宏。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_INIT_ELF='"$(OUT)/user/init.elf"'

# 大映像容量观察复用用户库，但保持独立main。
$(OUT)/user/capacity.elf: $(USER_COMMON) $(OUT)/user/capacity.o labs/airios/program.ld
# BSS只占memsz，不能把零填充强塞进内核输入文件。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/capacity.o -o $@
# 包装必须等到本配置容量程序链接完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/capacity.elf
# 原始ELF路径只由此对象的预处理变量消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CAPACITY_ELF='"$(OUT)/user/capacity.elf"'

# 用户SSE/x87观察由独立汇编保持活寄存器，不能用普通函数调用假定XMM保留。
$(OUT)/user/simd-probe.o: labs/airios/simd-probe.S Makefile
# 这个对象只进入用户ELF，与内核状态清理汇编分开。
	@mkdir -p $(dir $@)
# USER_FLAGS开放SSE2但不开放未保存的AVX。
	$(CC) $(USER_FLAGS) -c $< -o $@
# simd的main有真实double表达式；另链接寄存器观察汇编。
$(OUT)/user/simd.elf: $(USER_COMMON) $(OUT)/user/simd.o $(OUT)/user/simd-probe.o labs/airios/program.ld
# 静态链接仍只有一份main，不借宿主浮点运行库。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/simd.o $(OUT)/user/simd-probe.o -o $@
# reset探针在任何编译器SSE指令之前快照入口状态，因此此main保持general-only。
$(OUT)/user/simd-reset.o: labs/airios/simd-reset.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 与普通用户源使用同一配置目录。
	@mkdir -p $(dir $@)
# 后面的general-only只限制此观察C文件，内嵌FXSAVE64仍明确执行。
	$(CC) $(USER_FLAGS) -mgeneral-regs-only -Ilabs/airios/include -c $< -o $@
# 新ELF用于观察exec重新建立默认CPU状态。
$(OUT)/user/simd-reset.elf: $(USER_COMMON) $(OUT)/user/simd-reset.o labs/airios/program.ld
# 用户入口尚不触碰XMM，main第一件事保存真实状态。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/simd-reset.o -o $@
# 包装只等待本配置两份真实用户程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/simd.elf $(OUT)/user/simd-reset.elf
# 文件名以C字符串宏交给预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_SIMD_ELF='"$(OUT)/user/simd.elf"' -DAIRI_SIMD_RESET_ELF='"$(OUT)/user/simd-reset.elf"'

# 用户C与汇编也生成传递头文件依赖，Makefile变化会重编各对象，避免保留旧SSE编译策略。
-include $(wildcard $(OUT)/user/*.d)

# 两份FS观察用户程序分别保留自己的main，仍共用静态用户运行入口。
$(OUT)/user/tls.elf: $(USER_COMMON) $(OUT)/user/tls.o labs/airios/program.ld
# 普通用户C已允许SSE2，本探针的FS访问由明确内嵌汇编执行。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls.o -o $@
# reset只观察exec后FS默认值，不借用旧本地块。
$(OUT)/user/tls-reset.elf: $(USER_COMMON) $(OUT)/user/tls-reset.o labs/airios/program.ld
# 新ELF不与tls的main同时链接。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls-reset.o -o $@
# 包装只等待本配置的两份用户ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/tls.elf $(OUT)/user/tls-reset.elf
# 文件路径通过预处理宏进入.incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_TLS_ELF='"$(OUT)/user/tls.elf"' -DAIRI_TLS_RESET_ELF='"$(OUT)/user/tls-reset.elf"'

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

# POSIX风格包装是显式用户库，不让早期原始FS和4KiB模板实验强行加入errno。
$(OUT)/user/posix.o: labs/airios/libc/posix.c Makefile
# 使用同一用户目标与自动头依赖，拒绝宿主头文件实现混入。
	@mkdir -p $(dir $@)
# posix.c的_Thread_local错误槽由静态模板装载，库不自己设置FS。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 此程序单独选择posix.o，USER_COMMON仍只包含原始入口和字节/系统调用库。
$(OUT)/user/posix-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/posix-probe.o labs/airios/program.ld
# 无宿主libc，所有标准名字由本项目用户库提供。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/posix-probe.o -o $@
# 包装等待当前配置的完整用户ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/posix-probe.elf
# 该路径宏不改变内核调用号或符号。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_POSIX_ELF='"$(OUT)/user/posix-probe.elf"'

# 向量程序选择相同POSIX错误转换，内核仍使用独立固定宽度描述。
$(OUT)/user/vector-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/vector-probe.o labs/airios/program.ld
# 一份用户main与自有库，不调用宿主readv实现。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/vector-probe.o -o $@
# 包装等待当前配置的实际向量程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/vector-probe.elf
# 文件路径以C字符串宏交给预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_VECTOR_ELF='"$(OUT)/user/vector-probe.elf"'

# 纯块链算法再次以用户选项编译，内核heap.o不进入用户ELF。
$(OUT)/user/heap-algorithm.o: kernel/mm/heap.c Makefile
# 独立输出路径隔离两种编译策略。
	@mkdir -p $(dir $@)
# 算法只需要目标类型，隐式字节复制由用户bytes.o提供。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 对象包装单独选择errno与原始页来源，不改变早期无TLS程序。
$(OUT)/user/malloc.o: labs/airios/libc/malloc.c Makefile
# 与其他用户对象使用同一输出目录和传递头依赖。
	@mkdir -p $(dir $@)
# 所有状态位于当前用户ELF的BSS，编译器不访问内核memory.c。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 分配器观察是独立main，不把标准malloc塞进USER_COMMON。
$(OUT)/user/malloc-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/malloc-probe.o labs/airios/program.ld
# 原始库、POSIX错误槽、纯算法与用户对象包装都在这个静态ELF内。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 包装等待真实用户分配程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/malloc-probe.elf
# 本配置产物路径仅作为.incbin输入。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MALLOC_ELF='"$(OUT)/user/malloc-probe.elf"'

# 单独观察TLS零尾部与较小普通BSS，不需要先实现多个执行者。
$(OUT)/user/ctls-tail.elf: $(USER_COMMON) $(OUT)/user/ctls-tail.o labs/airios/program.ld
# 文件初值来自tdata，tbss运行空间不由普通LOAD长度替代。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始映像等当前配置的完整链接结果。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls-tail.elf
# 只读incbin路径在包装对象中消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_TAIL_ELF='"$(OUT)/user/ctls-tail.elf"'
```

</details>

<details>
<summary>labs/airios/images.S 的完整终点</summary>

<!-- write: labs/airios/images.S -->
```asm
.section .rodata.user_elf,"a" /* 原始ELF作为内核只读输入，不在内核直接执行。 */
.balign 8 /* 输入开头至少按8字节对齐，解析仍使用memcpy。 */
.global airi_hello_elf_start, airi_hello_elf_end /* 编译期固定映像边界。 */
airi_hello_elf_start: /* 文件头也包含在输入字节中。 */
.incbin AIRI_HELLO_ELF /* Make传入当前配置自己的ELF路径，不复制另一配置旧产物。 */
airi_hello_elf_end: /* 解析器只允许读取边界之内。 */
.section .note.GNU-stack,"",@progbits /* 包装对象不需要可执行栈。 */
.section .rodata.user_elf,"a" /* 第二个独立ELF同样只是内核装载输入。 */
.balign 8 /* 边界按八字节对齐，解析器仍安全按字节复制。 */
.global airi_process_elf_start, airi_process_elf_end /* 多进程用户工作负载的完整文件边界。 */
airi_process_elf_start: /* 包含独立文件头与程序表。 */
.incbin AIRI_PROCESS_ELF /* 路径由本配置的Make依赖提供。 */
airi_process_elf_end: /* 内核不能读过此边界。 */
.section .rodata.user_elf,"a" /* 第三个独立ELF作为exec目标。 */
.balign 8 /* 原始输入边界固定。 */
.global airi_worker_elf_start, airi_worker_elf_end /* 新用户映像范围。 */
airi_worker_elf_start: /* 解析从文件头开始。 */
.incbin AIRI_WORKER_ELF /* Make提供同一配置刚生成的文件。 */
airi_worker_elf_end: /* 输入长度不包括后续损坏夹具。 */
.global airi_broken_elf_start, airi_broken_elf_end /* 受控截断ELF不需要外部文件。 */
airi_broken_elf_start: /* 只有魔数与少量标识，不足64字节头。 */
.byte 0x7f, 0x45, 0x4c, 0x46, 2, 1, 1, 0 /* 解析器必须拒绝，不能改掉旧进程。 */
airi_broken_elf_end: /* 失败输入恰好八字节。 */

.section .rodata /* 文件工作负载只在内核中保留映像字节。 */
.global airi_files_elf_start, airi_files_elf_end /* 用户空间由装载器另建。 */
airi_files_elf_start: /* 映像首地址。 */
.incbin AIRI_FILES_ELF /* 使用本配置实际链接的用户ELF。 */
airi_files_elf_end: /* 映像右端，不把后续字节算入文件。 */

.section .rodata /* 存储用户映像属于只读内核初始目录。 */
.global airi_storage_elf_start, airi_storage_elf_end /* 同一映像按argv选择storage或recover。 */
airi_storage_elf_start: /* 文件首。 */
.incbin AIRI_STORAGE_ELF /* 本配置真实链接的用户ELF。 */
airi_storage_elf_end: /* 文件半开右端。 */

.section .rodata /* 独立管道用户映像。 */
.global airi_pipes_elf_start, airi_pipes_elf_end /* 只发布文件边界。 */
airi_pipes_elf_start: /* 文件起点。 */
.incbin AIRI_PIPES_ELF /* 当前配置的真实ELF。 */
airi_pipes_elf_end: /* 文件右端。 */

.section .rodata /* 阻塞用户程序映像。 */
.global airi_waits_elf_start, airi_waits_elf_end /* 边界只供内核装载。 */
airi_waits_elf_start: /* 文件开始。 */
.incbin AIRI_WAITS_ELF /* 本配置独立ELF。 */
airi_waits_elf_end: /* 半开右端。 */

.section .rodata /* 用户异步工作负载与exec别名共用同一ELF。 */
.global airi_async_elf_start, airi_async_elf_end /* 初始映像边界。 */
airi_async_elf_start: /* 文件首字节。 */
.incbin AIRI_ASYNC_ELF /* 本配置真实链接的用户程序。 */
airi_async_elf_end: /* 文件半开右端。 */

.section .rodata /* 新程序仍只是内核只读段中的输入文件。 */
.global airi_shell_elf_start, airi_shell_elf_end /* shell装载边界。 */
airi_shell_elf_start: /* 完整文件头起点。 */
.incbin AIRI_SHELL_ELF /* 当前配置的真实用户shell。 */
airi_shell_elf_end: /* 半开右端。 */
.global airi_demo_elf_start, airi_demo_elf_end /* 贯通演示装载边界。 */
airi_demo_elf_start: /* 完整ELF起点。 */
.incbin AIRI_DEMO_ELF /* 当前配置的真实异步用户程序。 */
airi_demo_elf_end: /* 半开右端。 */

.section .rodata /* 孤儿回收使用真正用户ELF，不在内核中直接执行。 */
.global airi_orphans_elf_start, airi_orphans_elf_end /* 初始目录只持有完整输入字节。 */
airi_orphans_elf_start: /* 文件头起点。 */
.incbin AIRI_ORPHANS_ELF /* 当前配置的独立ELF。 */
airi_orphans_elf_end: /* 文件半开右端。 */

.section .rodata /* 页释放程序由同一静态ELF装载器处理。 */
.global airi_memory_elf_start, airi_memory_elf_end /* 内核只保留输入字节边界。 */
airi_memory_elf_start: /* 文件首。 */
.incbin AIRI_MEMORY_ELF /* 本配置的新用户映像。 */
airi_memory_elf_end: /* 文件右端。 */

.section .rodata /* 常驻init仍由普通用户ELF装载路径启动。 */
.global airi_init_elf_start, airi_init_elf_end /* 发布文件边界，不在内核直接调用main。 */
airi_init_elf_start: /* 文件首。 */
.incbin AIRI_INIT_ELF /* 当前配置的独立静态ELF。 */
airi_init_elf_end: /* 文件右端。 */

.section .rodata /* 大BSS程序仍只嵌入实际文件，不嵌入运行时零填充页。 */
.global airi_capacity_elf_start, airi_capacity_elf_end /* 新初始映像的输入边界。 */
airi_capacity_elf_start: /* ELF文件首。 */
.incbin AIRI_CAPACITY_ELF /* 当前配置刚链接完成的独立用户程序。 */
airi_capacity_elf_end: /* 文件半开右端。 */

.section .rodata /* 浮点工作负载仍经过同一个静态ELF装载器。 */
.global airi_simd_elf_start, airi_simd_elf_end /* 主程序输入边界。 */
airi_simd_elf_start: /* 文件首。 */
.incbin AIRI_SIMD_ELF /* 当前配置带真实SSE/x87指令的ELF。 */
airi_simd_elf_end: /* 文件右端。 */
.global airi_simd_reset_elf_start, airi_simd_reset_elf_end /* exec新入口观察映像。 */
airi_simd_reset_elf_start: /* 文件首。 */
.incbin AIRI_SIMD_RESET_ELF /* 当前配置的状态快照程序。 */
airi_simd_reset_elf_end: /* 半开右端。 */

.balign 8 /* 两份FS观察程序仍是独立ELF字节，不是内核函数。 */
.global airi_tls_elf_start, airi_tls_elf_end /* 注册目录使用真实起止范围。 */
airi_tls_elf_start: /* 本配置的用户程序。 */
.incbin AIRI_TLS_ELF /* 路径由Makefile传入，不借宿主ABI。 */
airi_tls_elf_end: /* 当前ELF结束位置。 */
.balign 8 /* reset映像单独对齐。 */
.global airi_tls_reset_elf_start, airi_tls_reset_elf_end /* 成功exec的新目标。 */
airi_tls_reset_elf_start: /* 新映像独立入口。 */
.incbin AIRI_TLS_RESET_ELF /* main只查询新基址并输出。 */
airi_tls_reset_elf_end: /* 新ELF的右端。 */

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

.balign 8 /* POSIX风格包装仍在用户ELF中，不进内核ABI。 */
.global airi_posix_elf_start, airi_posix_elf_end /* 普通和reset目录项共用映像字节。 */
airi_posix_elf_start: /* 含errno静态TLS的新ELF。 */
.incbin AIRI_POSIX_ELF /* 只有该显式目标链接posix.o。 */
airi_posix_elf_end: /* 完整用户文件边界。 */

.balign 8 /* 向量用户程序同样经过独立ELF装载。 */
.global airi_vector_elf_start, airi_vector_elf_end /* 真实用户映像范围。 */
airi_vector_elf_start: /* 含POSIX包装的观察程序。 */
.incbin AIRI_VECTOR_ELF /* 本配置交叉链接产物。 */
airi_vector_elf_end: /* 只读输入的右端。 */

.balign 8 /* 对象分配器只进入选择它的用户ELF。 */
.global airi_malloc_elf_start, airi_malloc_elf_end /* 两个目录名共用这一完整映像。 */
airi_malloc_elf_start: /* 用户页与对象借用的真实观察程序。 */
.incbin AIRI_MALLOC_ELF /* 由本配置静态链接，不借宿主malloc。 */
airi_malloc_elf_end: /* 输入范围的排他右端。 */

.balign 8 /* TLS零尾部观察仍经真实静态ELF装载。 */
.global airi_ctls_tail_elf_start, airi_ctls_tail_elf_end /* 输入范围供真实初始程序目录使用。 */
airi_ctls_tail_elf_start: /* 普通BSS小于tbss的最小程序。 */
.incbin AIRI_CTLS_TAIL_ELF /* 当前配置真正链接的模板与LOAD描述。 */
airi_ctls_tail_elf_end: /* 完整文件的排他右端。 */
```

</details>

<details>
<summary>kernel/process/user.c 的完整终点</summary>

<!-- write: kernel/process/user.c -->
```c
#include <airi/time_abi.h> // 单调时间只输出固定数值，用户指针由当前根验证。
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
#include <airi/memory.h> // 页面、内核栈与长期根的所有权。
#include <airi/vm.h> // 每个进程有独立低半区。
#include <airi/timer.h> // 用户时间片依赖真实IRQ0。
#include <airi/kernel.h> // 有界串口输出与故障报告。
#define USER_CODE 0x400000ull // 旧受控映像的用户入口。
#define USER_STACK 0x70000000ull // 初始栈最高一页保持原地址，新增页向下展开。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_TLS 0x60000000ull // 静态TLS运行块不与ELF、动态堆和固定用户栈重叠。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
struct user_memory { // 用户地址空间及其页面所有权，不含CPU现场、内核栈、FD或异步owner。
    struct airi_address_space space; // 自有根和低半区页表，高半区继续借用。
    uint64_t records[RECORD_CHUNKS]; // 元数据页记录本地址空间拥有的数据页，不映射给用户。
    unsigned count; // 包括用户代码、动态页、TLS与用户栈，不包括内核陷入栈或FPU页。
};
_Static_assert(sizeof(struct user_memory) <= 128, "user memory owner must stay bounded"); // 按值转移只复制小描述，不复制1024页内容。
struct user_execution { // 一条用户执行链的CPU现场、内核陷入栈和等待条件。
    uint64_t fs_base; // 当前执行者的用户FS基址，不拥有对应用户数据页。
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
    struct airi_kernel_region stack; // 真正进入内核以后使用的栈，离开它才能销毁。
    unsigned user_ticks; // 本执行者实际收到的用户时钟事件。
    uint64_t fault_error; // 最近一次结束本执行者的用户故障错误位。
    enum user_state state; // 当前仍每进程一执行者，调度资格由这里决定。
    int64_t wait_target; // 等待的孩子PID条件，不是地址空间所有权。
    uint64_t wait_status; // wait结果的用户VA，只在当前等待期间借用。
    enum io_operation io; // 当前阻塞操作类型，不保存内核局部buffer。
    unsigned io_fd, io_events; // 当前I/O等待条件，FD本体归进程表。
    uint64_t io_address, io_bytes, io_deadline; // 当前操作借用的用户跨度和截止tick。
    struct airi_trap_frame initial; // 第一次进入用户时的寄存器值，不借临时栈。
    struct airi_trap_frame *frame; // 实际陷入以后指向本执行者内核栈上的现场。
};
struct user_process { // 当前仍为一个进程内嵌一条执行链，尚无线程共享与多执行者调度。
    struct user_memory memory; // 进程持有用户页表、数据页和元数据页。
    bool async_owned; // 当前PID的异步owner只由进程打开、reset和关闭。
    struct airi_fd_table files; // FD引用属于进程，exec保留、fork复制表项引用。
    unsigned pid, parent; // 进程身份与父关系不随CPU执行现场一起替换。
    int exit_code; // 父进程wait取得的进程退出结果。
    struct user_execution execution; // 暂时唯一的执行者，后续线程需要明确共享进程资源。
};
_Static_assert(sizeof(struct user_process) <= 1024, "exec control block must stay small"); // exec候选仍在内核栈上，大小不能随用户容量膨胀。
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
static unsigned io_block_count, io_wake_count, idle_halt_count; // 会话真实I/O阻塞与唤醒次数。
static bool session_idle, service_from_timer; // 是否暂回启动栈服务，和整个会话结束分开。
static unsigned supervisor_pid, input_pid; // 可选init身份与当前唯一UART输入身份，不是进程槽。
static bool session_active; // 只有用户会话活动时才接受用户IRQ或系统调用。
static int session_result; // 根用户退出结果，不用最后一个孩子的结果覆盖。
static struct airi_trap_frame *kernel_return; // 启动栈一直保留到整个用户会话结束。
extern const uint8_t airi_user_probe_start[], airi_user_probe_end[]; // 受控正常映像。
extern const uint8_t airi_user_bad_start[], airi_user_bad_end[]; // 用户cli故障。
extern const uint8_t airi_user_load_bad_start[], airi_user_load_bad_end[]; // 用户读supervisor页故障。
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
extern const uint8_t airi_ctls_tail_elf_start[], airi_ctls_tail_elf_end[]; // TLS零尾部大于普通LOAD的真实链接器布局。
extern const uint8_t airi_tls_big_elf_start[], airi_tls_big_elf_end[]; // 真正含超限PT_TLS的错误ELF。
extern const uint8_t airi_tls_elf_start[], airi_tls_elf_end[]; // FS本地数据与切换探针。
extern const uint8_t airi_tls_reset_elf_start[], airi_tls_reset_elf_end[]; // 成功exec清基址观察。
extern const uint8_t airi_simd_elf_start[], airi_simd_elf_end[]; // 浮点、抢占和继承探针。
extern const uint8_t airi_simd_reset_elf_start[], airi_simd_reset_elf_end[]; // exec新状态观察。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_capacity_elf_start[], airi_capacity_elf_end[]; // 大BSS、深用户栈与跨记录页的fork/exec。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
extern const uint8_t airi_demo_elf_start[], airi_demo_elf_end[]; // 用户页分配与异步存储贯通演示。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
    {"ctls-page", airi_ctls_page_elf_start, airi_ctls_page_elf_end}, // 两页运行块的真实边界。
    {"ctls-tail", airi_ctls_tail_elf_start, airi_ctls_tail_elf_end}, // 普通BSS与TLS零尾部各自分配。
    {"tls-too-big", airi_tls_big_elf_start, airi_tls_big_elf_end}, // 真实装载器必须拒绝，不进入用户main。
    {"tls", airi_tls_elf_start, airi_tls_elf_end}, // 每个用户身份独立FS基址。
    {"tls-reset", airi_tls_reset_elf_start, airi_tls_reset_elf_end}, // 新ELF不沿用旧FS。
    {"simd", airi_simd_elf_start, airi_simd_elf_end}, // 真实SSE/x87隔离与fork/exec。
    {"simd-reset", airi_simd_reset_elf_start, airi_simd_reset_elf_end}, // 新ELF入口的默认状态。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"async", airi_async_elf_start, airi_async_elf_end}, // 完整用户提交/完成路径。
    {"async-reset", airi_async_elf_start, airi_async_elf_end}, // 同一PID新epoch的验证入口。
    {"waits", airi_waits_elf_start, airi_waits_elf_end}, // 阻塞流和就绪条件等待。
    {"pipes", airi_pipes_elf_start, airi_pipes_elf_end}, // 有界FIFO与关闭语义。
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
    {"recover", airi_storage_elf_start, airi_storage_elf_end}, // 只读取已恢复记录。
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
static const struct executable *find_program(const char *name) { // 输入是内核已复制的短NUL字符串。
    size_t bytes = airi_strlen(name); // 只读当前有效字符串，不保存其指针。
    for (unsigned i = 0; i < sizeof(executables) / sizeof(executables[0]); ++i) // 遍历固定目录。
        if (airi_strlen(executables[i].name) == bytes && !airi_memcmp(executables[i].name, name, bytes)) // 长度与全部名称字节都相同才匹配。
            return &executables[i]; // 只借长期静态目录项。
    return NULL; // 未知名称不触碰任何用户地址空间。
}
static unsigned slot(const struct user_process *p) { // 只接受processes中的真实槽，不接受临时复制对象。
    return (unsigned)(p - processes); // 同一数组内指针差得到稳定槽号。
}
static bool create_stack(struct user_execution *execution, unsigned index) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + index * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&execution->stack, address, 4); // 完整成功后才借给用户根。
}
static struct user_page *owned_at(const struct user_memory *memory, unsigned index) { // 下标可等于count供新增项写入，但必须已有记录页。
    unsigned chunk = index / PAGE_RECORDS; // 整数除法选出记录页，不要求各物理页连续。
    if (index >= AIRI_USER_PAGES || !memory->records[chunk]) // 所有调用均来自内核已经验证的有效项。
        airi_panic("user page record is missing"); // 内部越界不能返回任意HHDM指针。
    struct user_page *entries = airi_memory_direct(memory->records[chunk]); // 别名只借用，物理所有权仍在records中。
    return &entries[index % PAGE_RECORDS]; // 余数选页内项；单项不会跨物理页。
}
static void trim_records(struct user_memory *memory) { // count已缩小，释放不再容纳任何活记录的尾部元数据页。
    unsigned needed = (memory->count + PAGE_RECORDS - 1) / PAGE_RECORDS; // 零项需要零页，其余向上取整。
    for (unsigned i = needed; i < RECORD_CHUNKS; ++i) { // 固定七槽，只检查完全位于有效项之外的页。
        if (!memory->records[i]) // 尚未分配的槽没有物理所有权。
            continue; // 不把0哨兵交给页池。
        if (!airi_memory_return_page(memory->records[i])) // 没有独立VA映射，HHDM随内核始终存在。
            airi_panic("cannot reclaim user page records"); // 归属错误不能静默复用页面。
        memory->records[i] = 0; // 归还后立即撤掉唯一所有权记录。
    }
}
static bool create_fpu(struct user_execution *execution) { // 构造器只给空控制块建立独立页所有权。
    execution->fpu_physical = airi_memory_take_page(); // 一页足够容纳对齐512字节状态，与用户记录页分别归还。
    if (!execution->fpu_physical) // 没有内存就不准备可运行身份。
        return false; // cleanup可回收此前的栈等资源。
    void *state = airi_memory_direct(execution->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由进程清理者持有。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
        return false; // 零长度也保留本项目地址策略。
    for (size_t done = 0; done < bytes;) { // 跨越的每一页都必须满足权限。
        unsigned flags; // 当前叶子权限。
        if (!airi_vm_lookup(&p->memory.space, address + done, NULL, &flags) || !(flags & AIRI_VM_USER) || (write && !(flags & AIRI_VM_WRITE))) // 输出指针还要求用户可写。
            return false; // 不先复制有效前缀。
        size_t part = 4096 - ((address + done) & 4095); // 当前页内剩余跨度。
        if (part > bytes - done) // 最后一段只到请求右端。
            part = bytes - done; // 不额外检查其他页。
        done += part; // 检查下一段。
    }
    return true; // 当前不可抢占内核中没有并发映射修改者。
}
static bool copy_user(struct user_process *p, void *buffer, uint64_t address, size_t bytes, bool to_user) { // 允许从非活动父根复制wait状态，但始终按其所有权查页。
    if (!user_access(p, address, bytes, to_user)) // 写用户缓冲必须检查WRITE，读只需要USER映射。
        return false; // 任何页失败都不复制。
    for (size_t done = 0; done < bytes;) { // 每次用HHDM别名处理一页中的有效段。
        uint64_t physical; // 翻译保留页内偏移。
        airi_vm_lookup(&p->memory.space, address + done, &physical, NULL); // 完整检查后页表在本次内核操作中保持不变。
        size_t part = 4096 - ((address + done) & 4095); // 当前页边界。
        if (part > bytes - done) // 不超出剩余内容。
            part = bytes - done; // 缩到最后一段。
        if (to_user) // wait结果由内核写入父进程自己的页面。
            airi_memcpy(airi_memory_direct(physical), (uint8_t *)buffer + done, part); // 不切到父用户根也能访问其已拥有物理页。
        else // write系统调用把用户数据复制进内核副本。
            airi_memcpy((uint8_t *)buffer + done, airi_memory_direct(physical), part); // 不信任原始VA作为内核指针。
        done += part; // 推进下一段。
    }
    return true; // 同步结束，不借出buffer指针。
}
static long file_transfer(struct user_process *p, uint64_t fd, uint64_t address, size_t bytes, bool write, bool positioned, uint64_t offset) { // 允许在另一根的阻塞进程上重试，同步副本属于当前内核栈。
    char buffer[256]; // 一次有界副本，不跨调度保留。
    if (fd >= AIRI_FD_COUNT) // 完整64位编号先检查再转换。
        return -9; // -EBADF。
    if (bytes > sizeof(buffer)) // 不让用户长度覆盖内核栈。
        return -22; // -EINVAL。
    if (!user_access(p, address, bytes, !write)) // 输出在消费流或偏移之前完整验证。
        return -14; // -EFAULT。
    if (write) { // 用户内容在本次尝试内复制。
        copy_user(p, buffer, address, bytes, false); // 通过目标自己的页表，不解引用原始用户VA。
        if (!positioned && airi_fd_console(&p->files, (unsigned)fd)) { // 固定终端同步发送。
            airi_serial_write_bytes(buffer, bytes); // 不保存局部buffer给设备。
            return (long)bytes; // 返回逻辑进展。
        }
        return airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // 满管道可返回EAGAIN。
    }
    if (airi_fd_input(&p->files, (unsigned)fd)) { // 固定输入对象的引用与当前前台读取资格分别检查。
        if (p->pid != input_pid) // fork复制的FD引用本身不转移UART消费权。
            return -13; // -EACCES；不消费设备字节。
        if (positioned) // 字节流没有文件偏移。
            return -29; // -ESPIPE，不消费字节。
        if (!bytes) // 零长度读取不能登记永久等待，也不消费UART。
            return 0; // 有效输入FD的空操作。
        size_t received = 0; // 返回当前可取得的前缀，不等待填满用户请求。
        while (received < bytes) { // bytes与整个用户输出范围已在上方验证。
            int c = airi_arch_serial_read(); // 设备寄存器只在架构层。
            if (c < 0) // 暂无字节和字符NUL不同。
                break; // 让已有前缀先交给用户。
            buffer[received++] = (uint8_t)c; // 固定内核副本不会越过256字节。
        }
        if (!received) // 空输入走同一个IO_READ持久等待机制。
            return -11; // 由陷入分派登记等待，不让shell忙轮询。
        if (!copy_user(p, buffer, address, received, true)) // 消费UART前已经完整验证用户范围。
            airi_panic("validated UART output mapping changed"); // 内部错误不能静默丢字节。
        return (long)received; // 实际取得的字节数。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
static long vector_transfer(struct user_process *p, uint64_t fd, uint64_t descriptors, uint64_t count, uint64_t offset, bool write) { // 当前只支持可定位文件，一次有界副本。
    if (fd >= AIRI_FD_COUNT) // 在窄化整数前检查完整FD。
        return -9; // -EBADF。
    if (count > AIRI_VECTOR_COUNT || offset > INT64_MAX) // 负count/offset转为无符号后也会在这里拒绝。
        return -22; // -EINVAL，不读取描述或改变文件。
    struct airi_iovec vectors[AIRI_VECTOR_COUNT]; // 当前调用持有的128字节描述快照。
    if (count && !copy_user(p, vectors, descriptors, (size_t)count * sizeof(vectors[0]), false)) // 整个数组必须可读，不在用户库中直接解引用。
        return -14; // -EFAULT，文件尚未操作。
    uint64_t total = 0; // 所有项串联的逻辑长度，不把溢出截成小请求。
    for (unsigned i = 0; i < count; ++i) { // 固定最多八项。
        if (vectors[i].bytes > (uint64_t)INT64_MAX - total) // 返回类型可表达范围在做加法前检查。
            return -22; // -EINVAL，拒绝负数式回绕或不可表示总量。
        total += vectors[i].bytes; // 总长仅决定本次有限前缀。
    }
    size_t bytes = total > AIRI_VECTOR_BYTES ? AIRI_VECTOR_BYTES : (size_t)total; // 长向量也只发起一次至多256字节操作。
    size_t planned = 0; // 本次会触及的前缀，越过256后的地址不访问。
    for (unsigned i = 0; i < count && planned < bytes; ++i) { // 先验证全部实际前缀，再进行任何I/O或输出。
        size_t part = vectors[i].bytes > bytes - planned ? bytes - planned : (size_t)vectors[i].bytes; // 一项可能只使用前半。
        if (part && !user_access(p, vectors[i].address, part, !write)) // 写文件读用户，读文件写用户；零长不检查无意义基址。
            return -14; // -EFAULT，前面合法项也未产生文件或输出副作用。
        planned += part; // 不用地址加长度形成可能回绕的右端。
    }
    uint8_t buffer[AIRI_VECTOR_BYTES]; // 同步调用内聚合/分散，不给设备DMA，不跨返回保留。
    size_t copied = 0; // gather按数组顺序形成同一文件操作的连续输入。
    if (write) // preadv不读取目标用户缓冲内容。
        for (unsigned i = 0; i < count && copied < bytes; ++i) { // 只聚合本次已验证前缀。
            size_t part = vectors[i].bytes > bytes - copied ? bytes - copied : (size_t)vectors[i].bytes; // 当前项实际长度。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, false)) // 映射在不可抢占、单线程内核中保持。
                airi_panic("validated vector input changed"); // 不变量破坏不能交出半份文件操作。
            copied += part; // 下一项紧随上一个实际字节。
        }
    long result = write ? airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, true, offset) : airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, true, offset); // 仅调用一次文件层，显式偏移不改共享位置。
    if (!write && result > 0) { // 只分散实际读到的字节，不用零覆盖尚未完成尾部。
        copied = 0; // 从第一个目标重新开始。
        for (unsigned i = 0; i < count && copied < (size_t)result; ++i) { // 本轮返回数不超过已验证256字节。
            size_t part = vectors[i].bytes > (size_t)result - copied ? (size_t)result - copied : (size_t)vectors[i].bytes; // EOF可能在项中间结束。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, true)) // 描述快照不受输出覆盖用户数组影响。
                airi_panic("validated vector output changed"); // 不能消费文件后掩盖复制失败。
            copied += part; // 未触及目标保留原有内容。
        }
    }
    return result; // 实际短数量、EOF0或原始负错误，不伪造全部完成。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 本阶段有界等待队列由进程槽直接表示。
        struct user_process *p = &processes[i]; // 控制块在等待期间不会回收。
        if (p->execution.state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (p->execution.io == IO_SLEEP) { // 只有截止tick，没有用户缓冲或设备FD。
            if (airi_timer_ticks() < p->execution.io_deadline) // 时钟尚未到期，保留原现场等待。
                continue; // 不恢复用户忙轮询。
            result = 0; // 到期后同一次sleep系统调用成功返回。
        } else if (p->execution.io == IO_ASYNC) { // 原CQ等待仍保留同一个用户输出地址。
            struct airi_io_completion completion; // 先取得内核数值副本，不借用户指针给引擎。
            result = airi_async_pop(p->pid, &completion); // 空CQ为-11，不代表I/O结果。
            if (result == -11) // 尚未形成可收集结果。
                continue; // 设备由启动服务栈推进，不在这里忙等。
            if (!result && !copy_user(p, &completion, p->execution.io_address, sizeof(completion), true)) // 等待前已验证，单线程阻塞期间映射不能改变。
                airi_panic("validated completion output mapping changed"); // 不能消费CQ后静默丢结果。
        } else if (p->execution.io == IO_READY) { // 订阅的是条件，不是一次成功read的承诺。
            result = airi_fd_ready(&p->files, p->execution.io_fd); // 当前条件可能被其他进程消费或改变。
            if (result >= 0) // 负错误必须原样唤醒，不能按位筛掉。
                result &= p->execution.io_events | AIRI_READY_HUP | AIRI_READY_ERROR; // 对端关闭即使未显式请求也应可见。
            if (!result && airi_timer_ticks() < p->execution.io_deadline) // 无条件成立且尚未超时。
                continue; // 保持等待，之后由状态变化或PIT服务重查。
        } else { // 阻塞read/write重新尝试同一个有界请求。
            result = file_transfer(p, p->execution.io_fd, p->execution.io_address, (size_t)p->execution.io_bytes, p->execution.io == IO_WRITE, false, 0); // 用户buffer仍由p持有。
            if (result == -11) // 暂时依旧没有进展。
                continue; // 不把原系统调用提前返回给用户忙等。
        }
        p->execution.frame->rax = (uint64_t)result; // 用户返回位置仍在原调用之后。
        p->execution.io = IO_NONE; // 这一次等待不再持有请求地址。
        p->execution.io_address = p->execution.io_bytes = p->execution.io_deadline = 0; // 清过期的用户对象与截止值。
        p->execution.state = USER_RUNNABLE; // 结果就绪之后才发布运行资格。
        ++io_wake_count; // 实际唤醒，不把查询次数计成完成。
        airi_log("[WAIT_WAKE] pid=%u result=%ld", p->pid, result); // 普通内核服务栈输出，不在IRQ里打印。
    }
}
static void destroy_memory(struct user_memory *memory) { // 目标根必须不活动，调用者已保存完需要的用户数值。
    if (memory->space.root && !airi_vm_destroy(&memory->space)) // 先去掉自有页表对数据页的映射，借用高半区不归还。
        airi_panic("cannot destroy inactive user tables"); // 不能继续释放仍被活动根引用的数据页。
    while (memory->count) // 页表已不再引用这些自有数据页。
        if (!airi_memory_return_page(owned_at(memory, --memory->count)->physical)) // 先退到最后一个真实有效项。
            airi_panic("cannot return owned user page"); // 内部归属错误不能静默复用。
    trim_records(memory); // 最后一个数据项归还后，元数据页不再有消费者。
    airi_memset(memory, 0, sizeof(*memory)); // 销毁结束不留下可重复归还的根或记录页句柄。
}
static struct user_memory take_memory(struct user_memory *source) { // 按值转移所有权，不复制用户页面也不改变CR3。
    struct user_memory value = *source; // 临时描述保存同一组根和记录页句柄。
    airi_memset(source, 0, sizeof(*source)); // 返回前撤销来源的归还资格，调用者必须接住返回值。
    return value; // 只有接收者继续拥有这些资源；转移本身不分配或失败。
}
static void destroy_execution(struct user_execution *execution) { // 调用者已离开这条内核栈，执行者也不能再恢复。
    if (execution->fpu_physical && !airi_memory_return_page(execution->fpu_physical)) // 用户地址空间不持有这份内核状态页。
        airi_panic("cannot reclaim user FPU state"); // 内部归属错误不能静默遗漏。
    if (execution->stack.count && !airi_memory_region_free(&execution->stack)) // 最后才能解除本执行者的内核陷入栈。
        airi_panic("cannot reclaim user kernel stack"); // 当前实际栈仍在使用时不得调用。
    airi_memset(execution, 0, sizeof(*execution)); // 清现场、等待借用和已归还句柄，不能再被选中。
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    if (p->async_owned) { // 失败构造或最终收拢只关闭真正拥有的owner。
        airi_async_close(p->pid); // ACTIVE副本归内核，取消不释放其后端目标。
        p->async_owned = false; // 重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 整进程清理才关闭全部表项，exec不调用此路径清旧内存。
    destroy_memory(&p->memory); // 只销毁用户映射和数据，不能触及本进程的执行栈。
    destroy_execution(&p->execution); // CPU状态页和陷入栈分别归执行者，最后统一撤掉恢复资格。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的身份或其他句柄。
}
static void initial_frame(struct user_process *p, uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
    if (name) { // C程序使用本项目最小argc/argv栈协议。
        size_t bytes = airi_strlen(name) + 1; // 包含用户字符串的终止零。
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&p->memory.space, USER_STACK, &physical, NULL)) // 栈必须已建立。
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
    p->execution.frame = &p->execution.initial; // 先保存独立寄存器值，place_frame稍后复制进实际内核栈。
    airi_memset(p->execution.frame, 0, sizeof(*p->execution.frame)); // 首次没有旧GPR。
    p->execution.frame->rip = entry; // ELF入口或已知汇编入口。
    p->execution.frame->cs = 27; // 用户代码段与RPL3。
    p->execution.frame->ss = 35; // 用户数据段与RPL3。
    p->execution.frame->rsp = stack_pointer; // 使用刚准备的用户布局。
    p->execution.frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_memory *memory, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (memory->count == AIRI_USER_PAGES) // 包括ELF、动态页和用户栈，不包括记录页。
        return false; // 不越过固定记录容量。
    unsigned chunk = memory->count / PAGE_RECORDS; // 下一项应写入哪一页元数据。
    if (!memory->records[chunk]) { // 仅跨过170项边界时才增加一个记录页。
        memory->records[chunk] = airi_memory_take_page(); // 记录页与用户数据页分别拥有、分别归还。
        if (!memory->records[chunk]) // 元数据耗尽不能继续分配没有记录位置的数据页。
            return false; // 旧有效项和旧映射完全保持。
        airi_memset(airi_memory_direct(memory->records[chunk]), 0, 4096); // 清除前一个所有者的地址记录。
    }
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) { // 可能刚借出记录页，却还未增加有效数据项。
        trim_records(memory); // 收回本次没有承载活项的新记录页。
        return false; // 调用方再回滚本批此前成功的数据页。
    }
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    if (!airi_vm_map(&memory->space, address, physical, flags | AIRI_VM_USER)) { // 单页映射失败还没有发布到所有权数组。
        airi_memory_return_page(physical); // 明确归还本次未接入数据页。
        trim_records(memory); // 映射失败也不能遗留空元数据页。
        return false; // 调用者回滚此前成功的部分。
    }
    *owned_at(memory, memory->count) = (struct user_page){address, physical, flags | AIRI_VM_USER}; // 保存VA/PA/权限供fork复制与精确回收。
    ++memory->count; // 记录完整后才发布有效项；指针不跨记录页释放保留。
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return -8; // -ENOEXEC，旧输出和调用者原上下文都未改变。
    }
    if (make_stack && !create_stack(&p->execution, slot(p))) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!create_fpu(&p->execution) || !airi_memory_user_space(&p->memory.space)) // 浮点页与独立低半区都必须完整建立。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
        unsigned flags = ((segment->flags & 2) ? AIRI_VM_WRITE : 0) | ((segment->flags & 1) ? AIRI_VM_EXEC : 0); // ELF PF标志转换成本内核权限枚举。
        uint64_t first = segment->address & ~4095ull; // 覆盖段首所在页。
        uint64_t end = (segment->address + segment->memory_bytes + 4095) & ~4095ull; // 包含完整BSS跨度。
        for (uint64_t address = first; address < end; address += 4096) // 所有页面先清零再接入。
            if (!owned_page(&p->memory, address, flags)) // 物理耗尽或映射失败都终止本次装载。
                goto failed; // 清理根/表后再归还数据页。
        for (uint64_t done = 0; done < segment->file_bytes;) { // 只复制filesz字节，memsz剩余继续保持零。
            uint64_t physical; // lookup结果包含当前页内偏移。
            if (!airi_vm_lookup(&p->memory.space, segment->address + done, &physical, NULL)) // 已映射跨度必须可查询。
                goto failed; // 内部装载状态不一致也不运行。
            uint64_t part = 4096 - ((segment->address + done) & 4095); // 当前页内可复制长度。
            if (part > segment->file_bytes - done) // 不从文件右端外复制字节。
                part = segment->file_bytes - done; // 最后一次缩到实际剩余。
            airi_memcpy(airi_memory_direct(physical), view.image + segment->offset + done, (size_t)part); // 内核控制的物理别名可初始化最终只读/可执行页。
            done += part; // 下一次继续剩余文件内容。
        }
    }
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 最高页地址不变，向低地址增加十五页。
        if (!owned_page(&p->memory, USER_STACK - i * 4096ull, AIRI_VM_WRITE)) // 栈RW/NX，最低页之下仍无映射。
            goto failed; // 任意一页失败均回收整份新映像。
    if (view.tls.memory_bytes) { // 无TLS程序继续FS=0，有模板才建立独立运行副本。
        for (unsigned i = 0; i < view.tls.pages; ++i) // 当前最多两页，含TP后的16字节TCB。
            if (!owned_page(&p->memory, USER_TLS + i * 4096ull, AIRI_VM_WRITE)) // 仍由用户页记录持有，RW/NX且fork逐页复制。
                goto failed; // 任何分配失败回滚整个新映像。
        uint64_t data = USER_TLS + view.tls.data_offset; // 小对齐模板可能在块首留下padding，保证负偏移相对TP正确。
        if (view.tls.file_bytes && !copy_user(p, (void *)(view.image + view.tls.offset), data, (size_t)view.tls.file_bytes, true)) // 按目标页查物理别名，初值可能跨页。
            goto failed; // 只复制filesz，memsz剩余保持owned_page的零填充。
        p->execution.fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        uint64_t control[2] = {p->execution.fs_base, 0}; // FS:0为self，第二字保留给后续自有运行库。
        if (!copy_user(p, control, p->execution.fs_base, sizeof(control), true)) // TCB属于当前新用户根，不能直接写未活动用户VA。
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->execution.fs_base); // 模板与运行位置分别可观察。
    }
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->memory.count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 长期根仍活动，用户空间从未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
    return -12; // -ENOMEM，不提供部分映像。
}
static void place_frame(struct user_execution *execution) { // 初始寄存器值完成后复制到自己拥有的内核栈。
    uint64_t top = execution->stack.virtual_address + execution->stack.count * 4096ull; // TSS以后从同一个栈顶压入真实现场。
    execution->frame = (void *)(uintptr_t)(top - sizeof(*execution->frame)); // 与公共入口的176字节布局一致。
    *execution->frame = execution->initial; // 不借用临时C结构或其他进程的内核栈。
}
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(&p->execution, slot(p))) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(&p->execution) || !airi_memory_user_space(&p->memory.space) || !owned_page(&p->memory, USER_CODE, AIRI_VM_EXEC) || !owned_page(&p->memory, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(&p->memory, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
    cleanup(p); // 完整回滚。
    return false; // 不运行半个上下文。
}
static struct user_process *find_pid(unsigned pid) { // PID与数组槽分开，退出回收后不会匹配旧身份。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历有界进程表。
        if (processes[i].execution.state != USER_UNUSED && processes[i].pid == pid) // 只查仍存活或待回收的身份。
            return &processes[i]; // 内核内部借用，不向用户交指针。
    return NULL; // 未找到活身份。
}
static struct user_process *unused(void) { // 准备中的槽不会被发布为RUNNABLE。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 固定容量避免无界内核分配。
        if (processes[i].execution.state == USER_UNUSED) // 调用者在不可抢占内核中独占准备。
            return &processes[i]; // 后续失败必须清掉全部所有权。
    return NULL; // 表满返回明确失败。
}
static void collect_pending(void) { // 只在新的用户陷入入口调用，不在旧进程退出的C调用中释放其栈。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 回收父进程已取得状态的孩子。
        if (&processes[i] != current && processes[i].execution.state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
            bool orphan = processes[i].parent == 0; // 用户wait回收与内核托管回收分开观察。
            cleanup(&processes[i]); // 非活动根和已离开的内核栈可安全解除。
            if (orphan) // 不把普通父wait冒充自动托管。
                airi_log("[ORPHAN_REAP] pid=%u released=1", pid); // 只在内存和FD资源实际释放后发布。
        }
}
enum schedule_reason { REASON_TIMER, REASON_YIELD, REASON_WAIT, REASON_EXIT, REASON_IO }; // 原因用于验证真实抢占，不混成主动yield。
static struct airi_trap_frame *select_user(struct user_process *p) { // 只在完整陷入现场边界切根和TSS。
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->execution.state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->execution.stack.virtual_address + p->execution.stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->memory.space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->execution.frame); // 用户返回地址与标志必须仍符合边界。
}
static struct airi_trap_frame *pause_for_service(bool from_timer) { // 完整用户frame已保存，暂回长期启动栈做可延后工作。
    session_idle = true; // 会话仍active，不是用户程序已经退出。
    service_from_timer = from_timer; // 真正IRQ导致的换身份计数另记。
    airi_memory_use_kernel(); // 启动服务不依赖某个用户根。
    kernel_return->rax = 0; // 暂停结果不当成应用最终退出值。
    return kernel_return; // 汇编离开当前进程栈，之后才能安全服务其他等待者。
}
static struct airi_trap_frame *schedule(enum schedule_reason reason) { // 当前完整frame边界可切换，但IRQ不直接执行复杂I/O。
    struct user_process *old = current; // 仍暂用old的内核栈。
    if (old->execution.state == USER_RUNNING) // 等待或退出不能误改回可运行。
        old->execution.state = USER_RUNNABLE; // 时间片结束仍可运行。
    if (reason != REASON_TIMER) // 普通内核边界可重试等待条件。
        poll_waiters(); // 已有字节、关闭或错误形成后再唤醒。
    else { // IRQ只检查状态，并把设备推进交给启动服务栈。
        bool service = airi_async_work(); // 即使无人等待，已接收请求也不能被纯计算用户饿死。
        for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 管道、有限条件与CQ等待都需要普通服务。
            service |= processes[i].execution.state == USER_IO_WAIT; // 不在IRQ里复制、分配或打印。
        if (service) // 完整用户frame已经保存。
            return pause_for_service(true); // 由启动栈pump与重查，再进入下一个用户。
    }
    for (unsigned n = 1; n <= PROCESS_COUNT; ++n) { // 从old之后轮转一圈。
        struct user_process *next = &processes[(slot(old) + n) % PROCESS_COUNT]; // PID与槽仍分开。
        if (next->execution.state == USER_RUNNABLE) { // 仅可运行者有可返回的用户现场。
            if (next != old && reason == REASON_TIMER) // 真IRQ切不同身份。
                ++timer_switches; // 主动阻塞不冒充抢占。
            return select_user(next); // 汇编恢复目标frame。
        }
    }
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 没有可运行者时检查仍在等待的活进程。
        if (processes[i].execution.state == USER_WAITING || processes[i].execution.state == USER_IO_WAIT) // 可能正在等设备、超时或未来对端。
            return pause_for_service(false); // 允许启动栈hlt等待真实IRQ，不PANIC用户死锁。
    session_active = false; // 所有用户均结束，此时才算完整会话退出。
    session_idle = false; // 不再保留暂停身份。
    airi_memory_use_kernel(); // 最后一个用户根也变成不活动。
    kernel_return->rax = (uint64_t)(int64_t)session_result; // 根用户的真实退出结果。
    return kernel_return; // 回启动栈后统一回收残留资源。
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前进程内核栈上，先结束身份而不释放栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    if (p->async_owned) { // 用户身份结束，不能把旧CQ交给之后复用的进程槽。
        airi_async_close(p->pid); // QUEUED撤销，ACTIVE内核副本保持到收集。
        p->async_owned = false; // cleanup不重复关闭未来的新epoch。
    }
    if (input_pid == p->pid) // 当前前台shell停止后，init的新孩子才能取得UART。
        input_pid = 0; // 只释放本进程的消费权，不替其他身份撤销。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->execution.state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
        session_result = code; // 孩子退出不改变根结果。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 父退出时交接全部直接孩子，不等待整次会话收拢。
        struct user_process *child = &processes[i]; // 活孩子和未收的僵尸都可能仍引用此父PID。
        if (child->execution.state == USER_UNUSED || child->parent != p->pid) // 只接管本次退出者真正拥有的孩子。
            continue; // 不改其他父子关系。
        child->parent = 0; // PID0表示内核托管者，不是可运行的用户进程。
        if (child->execution.state == USER_ZOMBIE) // 父已无法取得这份退出状态。
            child->execution.state = USER_REAP_PENDING; // 下一条安全内核栈回收，不让僵尸永久占槽。
        airi_log("[ORPHAN_ADOPT] pid=%u former_parent=%u finished=%u", child->pid, p->pid, child->execution.state == USER_REAP_PENDING); // 活孩子仍正常调度或等待。
    }
    struct user_process *parent = find_pid(p->parent); // 父身份可能已退出或不存在。
    if (p->pid != root_pid && (!parent || parent->execution.state == USER_ZOMBIE || parent->execution.state == USER_REAP_PENDING)) { // 会话根的统计保留到launch结束；普通孤儿无需用户wait。
        p->parent = 0; // 不再保留已失效的用户父PID。
        p->execution.state = USER_REAP_PENDING; // 当前栈仍在使用，只登记延后回收资格。
        airi_log("[ORPHAN_EXIT] pid=%u code=%d deferred=1", p->pid, code); // 真正释放发生在离开此栈后。
    }
    if (parent && parent->execution.state == USER_WAITING && (parent->execution.wait_target == -1 || (uint64_t)parent->execution.wait_target == p->pid)) { // 只有真正等待此孩子的父进程可以被唤醒。
        bool copied = !parent->execution.wait_status || copy_user(parent, &code, parent->execution.wait_status, sizeof(code), true); // 通过父进程自己的页表写结果。
        parent->execution.frame->rax = copied ? p->pid : (uint64_t)-14ll; // 返回子PID或明确地址错误。
        parent->execution.state = USER_RUNNABLE; // 结果形成后才公开可运行状态。
        parent->execution.wait_target = 0; // 这一次等待已经结束。
        parent->execution.wait_status = 0; // 不保留用户局部对象的过期地址。
        if (copied) // 地址错误时保留僵尸，让父进程能用有效指针重试。
            p->execution.state = USER_REAP_PENDING; // 下一次别的用户陷入后才能释放本次仍在用的内核栈。
    }
    airi_log("[USER_EXIT] pid=%u code=%d user_ticks=%u", p->pid, code, p->execution.user_ticks); // 正常系统调用上下文记录身份结果。
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    if (next_pid == UINT32_MAX) // 固定32位正PID不允许回绕命中旧身份。
        return -75; // -EOVERFLOW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(&child->execution, slot(child)) || !create_fpu(&child->execution) || !airi_memory_user_space(&child->memory.space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->memory.count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(&parent->memory, i); // 每页原VA、权限和物理地址。
        if (!owned_page(&child->memory, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(&child->memory, child->memory.count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    child->execution.fs_base = parent->execution.fs_base; // fork继承同一用户VA，指向复制后的子页面，不共享物理内容。
    airi_memcpy(airi_memory_direct(child->execution.fpu_physical), airi_memory_direct(parent->execution.fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->execution.initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->execution.initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
    place_frame(&child->execution); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->execution.state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->memory.count); // 实际复制数量含用户栈与堆。
    return child->pid; // 父分支得到正子PID。
failed: // 还没有向调度器发布孩子，当前仍在父根和父内核栈上。
    cleanup(child); // 删除不活动子根和其独立栈，父继续原系统调用。
    return -12; // -ENOMEM，父的地址空间和返回点不变。
}
static struct airi_trap_frame *wait_current(struct airi_trap_frame *f) { // 自有wait ABI返回PID，status写普通int而非Linux编码。
    int64_t target = (int64_t)f->rdi; // -1表示任意孩子，其他必须为正PID。
    uint64_t status = f->rsi; // NULL允许只回收不读取退出码。
    if ((target < 1 && target != -1) || (status && !user_access(current, status, sizeof(int), true))) { // 输出范围在阻塞之前就核对。
        f->rax = (target < 1 && target != -1) ? (uint64_t)-22ll : (uint64_t)-14ll; // 参数错误不改变调度状态。
        return airi_user_resume(f); // 当前进程仍运行，可换参数重试。
    }
    bool found = false; // 区分没有孩子与孩子仍在运行。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 只匹配当前PID的子关系。
        struct user_process *child = &processes[i]; // 槽可能已经回收或属于其他父进程。
        if (child->execution.state == USER_UNUSED || child->execution.state == USER_REAP_PENDING || child->parent != current->pid || (target != -1 && (uint64_t)target != child->pid)) // 不允许等待别人的子进程。
            continue; // 查找下一个可能孩子。
        found = true; // 至少有一个真实孩子。
        if (child->execution.state == USER_ZOMBIE) { // 已结束孩子可以在父内核栈上立即回收。
            unsigned pid = child->pid; // 清理前保存返回身份。
            int code = child->exit_code; // 清理前保存退出状态。
            if (status && !copy_user(current, &code, status, sizeof(code), true)) // 即使前面检查过也保留明确复制结果。
                airi_panic("wait output mapping changed in nonpreemptible kernel"); // 内部不变量被破坏，不静默错写。
            cleanup(child); // 当前确实不在孩子栈上，其根也不活动。
            f->rax = pid; // 回收完成后返回已取得状态的孩子PID。
            return airi_user_resume(f); // 普通返回到父用户调用方。
        }
    }
    if (!found) { // 没有匹配孩子，不应永远等待。
        f->rax = (uint64_t)-10ll; // -ECHILD。
        return airi_user_resume(f); // 父可继续其他工作。
    }
    current->execution.wait_target = target; // 保存要等待的身份条件。
    current->execution.wait_status = status; // 父阻塞期间单线程不会修改这些用户映射。
    current->execution.state = USER_WAITING; // 先记录条件与结果地址，再让调度器跳过父。
    ++wait_blocks; // 真实阻塞，而非轮询返回成功。
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
static long allocate_user(size_t bytes) { // 按页提供有界用户内存，不是完整malloc。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull) // 一次最多六十四页，取整加法保持有界。
        return -22; // -EINVAL。
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > AIRI_USER_PAGES - current->memory.count) // 先检查整批有效项容量。
        return -12; // -ENOMEM。
    uint64_t address = USER_HEAP; // 从低端找能容纳本次整段的空洞，不依赖旧分配顺序。
    for (;;) { // 每次冲突越过至少一个已拥有页，搜索被1024项预算限制。
        bool conflict = false; // 本候选范围是否撞上现有用户数据页。
        for (unsigned i = 0; i < current->memory.count; ++i) { // owned可能因释放而换序，不要求按VA排序。
            uint64_t used = owned_at(&current->memory, i)->address; // 当前页仍由此进程拥有。
            if (used >= address && used < address + count * 4096ull) { // 页对齐后，页起点落入候选即发生重叠。
                address = used + 4096; // 此页之前的起点都不能容纳本次整段，跳到该页右端。
                conflict = true; // 新候选须重新从整个owned数组检查。
                break; // 不沿用旧范围检查结果。
            }
        }
        if (address > USER_HEAP_END - count * 4096ull) // 先减有界长度，不允许候选越过动态范围。
            return -12; // -ENOMEM；尚未分配页面，也没有修改原映射。
        if (!conflict) // 所有现有页都不与此候选相交。
            break; // 后续只为这段空洞创建映射。
    }
    unsigned before = current->memory.count; // 失败只回滚本次新增映射。
    for (unsigned i = 0; i < count; ++i) // 页物理可以不连续，用户VA连续。
        if (!owned_page(&current->memory, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 游标尚未前移。
    airi_log("[USER_ALLOC] pid=%u address=0x%llx pages=%u zeroed=1", current->pid, (unsigned long long)address, count); // 只记录已完整提交的用户映射。
    return (long)address; // 用户可显式归还完整页，未归还页仍在exit统一清理。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->memory.count > before) { // 只处理新增的成功项。
        const struct user_page *page = owned_at(&current->memory, --current->memory.count); // 先退到最后一项。
        if (!airi_vm_unmap(&current->memory.space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    trim_records(&current->memory); // 本批回滚后，尾部没有活项的记录页也一起归还。
    return -12; // 不返回半段用户范围。
}
static unsigned owned_index(uint64_t address) { // 只在当前单线程进程的不可抢占系统调用中查询。
    for (unsigned i = 0; i < current->memory.count; ++i) // 有效项之外可能有旧字节，不参与查找。
        if (owned_at(&current->memory, i)->address == address) // 精确页起点，不接受页内指针。
            return i; // 下标只借给本次操作，不跨状态修改保存。
    return current->memory.count; // count本身是未找到哨兵，不能作为数组下标解引用。
}
static long release_user(uint64_t address, uint64_t bytes) { // 按页解除动态用户映射，不是C malloc对象free。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull || (bytes & 4095) || (address & 4095) || address < USER_HEAP || address >= USER_HEAP_END || bytes > USER_HEAP_END - address) // 短路先限地址再做右端减法。
        return -22; // -EINVAL；不舍入无效长度，不动任何页。
    unsigned count = (unsigned)(bytes / 4096); // 参数已限制为一到六十四个完整页。
    for (unsigned i = 0; i < count; ++i) { // 第一遍先验证整段，避免中途坏页留下部分解除。
        unsigned index = owned_index(address + i * 4096ull); // 当前目标VA属于动态页范围。
        if (index == current->memory.count || owned_at(&current->memory, index)->flags != (AIRI_VM_USER | AIRI_VM_WRITE)) // 既需真实所有权，也只接受RW/NX用户动态页。
            return -22; // 任何一页不存在，整段保持原样。
    }
    for (unsigned i = 0; i < count; ++i) { // 第二遍提交，当前没有其他线程改变本地址空间。
        unsigned index = owned_index(address + i * 4096ull); // 每次重新查，上一轮可能移动了owned末项。
        struct user_page page = *owned_at(&current->memory, index); // 在压缩数组前保留当前VA与PA数值。
        if (!airi_vm_unmap(&current->memory.space, page.address) || !airi_memory_return_page(page.physical)) // 先断开活动根映射并invlpg，才归还物理页。
            airi_panic("cannot release owned dynamic user page"); // 内部不变量失败不能返回假成功。
        *owned_at(&current->memory, index) = *owned_at(&current->memory, --current->memory.count); // 末项可位于另一记录页，复制后才允许回收尾页。
    }
    trim_records(&current->memory); // 全段压缩结束，任何有效项都不再借用空尾页。
    airi_log("[USER_RELEASE] pid=%u address=0x%llx pages=%u", current->pid, (unsigned long long)address, count); // 只记录整段真正归还后的结果。
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
    bool terminated = false; // 不能把未终止输入交给strlen。
    for (unsigned i = 0; i < sizeof(name); ++i) { // 有界逐字节读取，允许跨用户页但不越过上限。
        if (f->rdi > UINT64_MAX - i || !copy_user(current, &name[i], f->rdi + i, 1, false)) // 名称本身也可能是坏指针。
            return -14; // -EFAULT，旧用户继续。
        if (!name[i]) { // 取得完整NUL字符串后才查目录。
            terminated = true; // 后续strlen有明确终止界限。
            break; // 不读取字符串后无关字节。
        }
    }
    if (!terminated) // 超过本项目名称预算。
        return -36; // -ENAMETOOLONG。
    const struct executable *image = find_program(name); // 不存在目标不用分配新根。
    if (!image) // 和损坏格式是两种不同失败。
        return -2; // -ENOENT。
    bool acquire_input = supervisor_pid && current->parent == supervisor_pid && airi_strlen(name) == 2 && !airi_memcmp(name, "sh", 2); // 只有init的直接shell孩子可获得新前台输入。
    if (acquire_input && input_pid && input_pid != current->pid) // 同一个init也不能同时授权两个读者。
        return -16; // -EBUSY；尚未触碰旧映像或FD。
    struct user_process next = {0}; // 临时所有者不在进程表中，尚不能被调度。
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    int async_error = airi_async_reset(current->pid); // 新ELF完整后，提交前确认可以进入新资源epoch。
    if (async_error) { // reset失败发生在撤销前，旧映像与旧I/O资源仍有效。
        cleanup(&next); // next尚未拥有当前PID的owner。
        return async_error; // 原用户调用可继续处理失败。
    }
    unsigned old_pages = current->memory.count; // 清理前保留替换规模，不复制整进程控制块。
    struct user_memory old_memory = take_memory(&current->memory); // 旧根此刻仍活动，其所有权暂由局部描述接住。
    current->memory = take_memory(&next.memory); // 新根所有权进入原进程槽，候选来源立即为空。
    uint64_t old_fpu = current->execution.fpu_physical; // 旧用户CPU状态页不属于用户地址空间。
    current->execution.fpu_physical = next.execution.fpu_physical; // 替换为准备好的默认x87/SSE状态。
    next.execution.fpu_physical = 0; // 候选从此不能再归还这页。
    current->execution.fs_base = next.execution.fs_base; // 新映像的静态TLS指针，或无TLS时的0。
    current->execution.initial = next.execution.initial; // 保存新入口数值，不保存指向候选栈局部对象的指针。
    current->execution.frame = f; // 原内核陷入栈及TSS继续属于同一执行者。
    *f = current->execution.initial; // 全部可失败准备完成之后才覆盖旧RIP/RSP和寄存器。
    current->execution.fault_error = 0; // 新映像没有旧用户故障报告。
    current->exit_code = 0; // 运行中的新映像还没有退出结果。
    current->execution.wait_target = 0; // 原先完成的wait条件不能成为新映像状态。
    current->execution.wait_status = 0; // 撤销旧用户输出地址的数值借用。
    current->execution.io = IO_NONE; // exec发生在运行系统调用中，没有需要继续的旧阻塞操作。
    current->execution.io_fd = current->execution.io_events = 0; // FD表原地保留，但旧等待条件清零。
    current->execution.io_address = current->execution.io_bytes = current->execution.io_deadline = 0; // 不把旧空间中的请求指针带到新映像。
    airi_fd_close_all(&current->files, true); // 进程FD表原地保留，成功提交才关闭CLOEXEC。
    if (input_pid == current->pid) // 若前台进程自行exec，旧输入权限不能漏到其他映像。
        input_pid = 0; // 成功提交时才撤销，失败exec仍保留旧权限。
    if (acquire_input) { // 本次新映像确实是init管理的shell。
        airi_fd_close(&current->files, 0); // 无表项时-9可忽略；已有普通FD0也按此启动策略关闭。
        if (!airi_fd_attach_input(&current->files)) // 新FD0必须为空且输入引用成功接入。
            airi_panic("cannot attach supervised shell input"); // 事务提交内部不变量错误。
        input_pid = current->pid; // 文件引用完成后才发布唯一消费身份。
    }
    current->execution.state = USER_RUNNING; // PID、父关系、已运行tick、内核栈和异步owner均原地保持。
    if (!airi_vm_activate(&current->memory.space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    destroy_memory(&old_memory); // CR3已切到新根，只归还局部接住的旧用户内存。
    if (!airi_memory_return_page(old_fpu)) // 硬件随后从新状态页恢复，不再借旧页指针。
        airi_panic("cannot reclaim replaced user FPU state"); // 内部归还错误不能伪装成普通exec失败。
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->memory.count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
    if (!session_active || !current || current->execution.state != USER_RUNNING || (f->cs & 3) != 3) // 当前实际栈必须有运行所有者。
        airi_panic("CPU state without a running user"); // 内核int129不经过这个入口。
    current->execution.fs_base = airi_tls_take(); // 先取真实用户基址，再把硬件FS清零供内核处理。
    airi_fpu_save(airi_memory_direct(current->execution.fpu_physical)); // x87/XMM也在此唯一入口保存，不能因设备没有新ISR而跳过。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", current->pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 不解引用错误地址。
        return exit_current(-22); // 只结束拥有这个现场的用户。
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    airi_tls_restore(current->execution.fs_base); // 与所选用户身份一起恢复，随后内核C不会通过FS访问数据。
    airi_fpu_restore(airi_memory_direct(current->execution.fpu_physical)); // 当前身份已确定，恢复后内核和公共汇编不再使用浮点/XMM。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->execution.state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->execution.frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
    current->execution.state = USER_RUNNABLE; // 不丢掉被设备提示打断的计算用户运行资格。
    return pause_for_service(false); // 切到长期普通栈后才pump、复制用户输出和唤醒等待者。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->execution.state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    current->execution.frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->execution.user_ticks; // 实际CPU用户时间片事件数。
    if (current->execution.user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
        return schedule(REASON_TIMER); // 没有其他可运行者时恢复同一个进程。
    return airi_user_resume(f); // 也不能跳过坏返回检查。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // 内核仍不可抢占，只在完整用户陷入边界处理进程状态。
    if (f->vector == 129 && f->cs == 8 && (!session_active || session_idle) && current && current->execution.state == USER_RUNNABLE) { // 内核启动一组已经准备好的用户。
        kernel_return = f; // 启动栈的继续位置一直保留。
        session_idle = false; // 当前int129重新登记了仍在启动栈上的继续位置。
        session_active = true; // 之后的CS3陷入有会话归属。
        return select_user(current); // 按目标根和TSS进入首个用户。
    }
    if (!session_active || !current || current->execution.state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->execution.frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
        current->execution.fault_error = f->error; // 保存权限故障证据。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // 同步文件与流的完整副本边界。
        bool write = f->rax == 1 || f->rax == 16; // 输出到流或文件。
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
        long result = file_transfer(current, f->rdi, f->rsi, (size_t)f->rdx, write, positioned, f->r10); // 先实际尝试一次。
        if (result == -11 && !positioned && !airi_fd_nonblocking(&current->files, (unsigned)f->rdi)) { // 阻塞流暂不可完成，用户不能提前看到EAGAIN。
            current->execution.io = write ? IO_WRITE : IO_READ; // 保存需要重试的操作种类。
            current->execution.io_fd = (unsigned)f->rdi; // 此时FD已由file_transfer完整验证。
            current->execution.io_address = f->rsi; // 用户页继续归此进程所有，不借内核局部buffer。
            current->execution.io_bytes = f->rdx; // 一次最多256字节，已经验证跨度。
            current->execution.state = USER_IO_WAIT; // 条件和数据先登记，再移出可运行集合。
            ++io_block_count; // 实际阻塞计数。
            airi_log("[WAIT_REGISTER] pid=%u operation=%s fd=%u", current->pid, write ? "write" : "read", current->execution.io_fd); // 普通系统调用上下文记录。
            return schedule(REASON_IO); // 选择别的用户或进入真正内核空闲等待。
        }
        f->rax = (uint64_t)result; // 非阻塞仍把EAGAIN交给用户，其他结果直接返回。
    } else if (f->rax == 2) // 退出后不再恢复此用户指令。
        return exit_current((int)(f->rdi & 255)); // 普通退出码0–255。
    else if (f->rax == 3) // getpid查询当前身份。
        f->rax = current->pid; // 槽复用不改变已运行PID语义。
    else if (f->rax == 4) // 显式yield让出一次时间片。
        return schedule(REASON_YIELD); // 和真实timer抢占分开计数。
    else if (f->rax == 5) // 单调tick，不是墙钟。
        f->rax = airi_timer_ticks(); // IRQ在用户执行期间更新。
    else if (f->rax == 6) // fork复制所有用户页与返回现场。
        f->rax = (uint64_t)fork_current(f); // 父得到子PID，孩子自己的frame已经设为0。
    else if (f->rax == 7) { // exec失败保留旧现场，成功已把f替换成新入口。
        long result = exec_current(f); // 名称复制与新空间事务在不可抢占内核完成。
        if (result < 0) // 只有失败才向旧用户调用返回错误。
            f->rax = (uint64_t)result; // PID和旧地址空间仍保持。
        return airi_user_resume(f); // 成功恢复新_start，失败恢复旧调用下一条指令。
    } else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else if (f->rax == 11) { // open只消费内核持有的短路径副本。
        char path[32]; // 与平坦目录名容量一致。
        bool copied = false; // 必须看到终止零才可交给文件层。
        for (size_t i = 0; i < sizeof(path); ++i) { // 逐字节检查，不能strlen用户指针。
            if (!copy_user(current, &path[i], f->rdi + i, 1, false)) // 未映射或内核地址拒绝。
                break; // 不发生创建或截断。
            if (!path[i]) { // 有界NUL字符串完成。
                copied = true; // 副本可在当前内核调用中使用。
                break; // 不额外读取终止零后面的用户页。
            }
        }
        f->rax = copied && f->rsi <= 31 ? (uint64_t)airi_fd_open(&current->files, path, (unsigned)f->rsi) : (uint64_t)-14ll; // 错误路径不变更文件。
    } else if (f->rax == 12) // close撤销当前表项引用。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_close(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll); // 不删除文件内容。
    else if (f->rax == 13) // dup共享打开对象，标志放在新表项。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_seek(&current->files, (unsigned)f->rdi, f->rsi) : (uint64_t)-9ll); // 共享打开对象的偏移随之变化。
    else if (f->rax == 17 || f->rax == 18) { // 日志append与read不是通用文件ABI。
        uint8_t buffer[480]; // payload上限与磁盘格式一致，设备不直接访问用户页。
        long result; // 实际长度、sequence或负错误。
        if (f->rax == 17) { // RDI输入地址，RSI长度。
            if (!f->rsi || f->rsi > sizeof(buffer)) // 不创建空记录或无界内核副本。
                result = -22; // -EINVAL。
            else if (!copy_user(current, buffer, f->rdi, (size_t)f->rsi, false)) // 完整复制后才可能产生磁盘副作用。
                result = -14; // -EFAULT。
            else { // 设备同步等待临时开IRQ，但不会在CS0切用户。
                airi_log("[STORAGE_REQUEST] pid=%u operation=append bytes=%llu wait=kernel", current->pid, (unsigned long long)f->rsi); // 诚实区分同步内核等待与后续用户阻塞。
                result = airi_storage_append(buffer, (size_t)f->rsi); // 成功已完成数据和提交同步。
            }
        } else { // RDI索引，RSI用户输出，RDX容量。
            if (f->rdi >= 64 || f->rdx > sizeof(buffer)) // 完整64位索引不能截断成其他记录。
                result = -22; // -EINVAL。
            else if (!user_access(current, f->rsi, (size_t)f->rdx, true)) // 先验证完整用户输出范围。
                result = -14; // -EFAULT，不进行设备I/O。
            else { // 仅将校验通过的实际长度复制到用户。
                result = airi_storage_read((unsigned)f->rdi, buffer, (size_t)f->rdx); // 设备数据和commit重新核对。
                if (result > 0 && !copy_user(current, buffer, f->rsi, (size_t)result, true)) // 当前映射不应变化。
                    airi_panic("validated storage output mapping changed"); // 内部不变量破坏明确停止。
            }
        }
        f->rax = (uint64_t)result; // 同一次用户调用取得精确结果。
    } else if (f->rax == 19) // 已恢复前缀数值查询。
        f->rax = (uint64_t)airi_storage_count(); // 缺设备或不确定状态返回负数。
    else if (f->rax == 20) { // pipe原子创建并返回两个整数，不暴露内部环。
        unsigned ends[2]; // 内核副本，发布前用户输出已验证。
        long result; // 0或负错误。
        if (f->rsi > 1) // 明确接受阻塞0与非阻塞1，其他参数拒绝。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(ends), true)) // 坏输出不能创建无法交给用户的FD。
            result = -14; // -EFAULT。
        else { // 没有半次FD分配的用户可见状态。
            result = airi_fd_pipe(&current->files, ends, f->rsi != 0); // 两端和缓冲都齐后才成功。
            if (!result && !copy_user(current, ends, f->rdi, sizeof(ends), true)) // 映射在本次调用中不应变化。
                airi_panic("validated pipe output mapping changed"); // 明确内核不变量。
        }
        f->rax = (uint64_t)result; // 失败输出保持原值。
    } else if (f->rax == 21) // 当前就绪条件查询，不保存通知事件。
        f->rax = f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_ready(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll; // 完整64位编号先限制。
    else if (f->rax == 22) { // 等待管道条件，有限tick超时，0表示只查询。
        long result; // 事件掩码、0超时或负错误。
        if (f->rdi >= AIRI_FD_COUNT || !f->rsi || (f->rsi & ~3ull) || f->rdx > 500) // 只接受READ/WRITE请求，最多五秒。
            result = -22; // -EINVAL。
        else { // 先看当前条件，避免已经满足还去睡眠。
            result = airi_fd_ready(&current->files, (unsigned)f->rdi); // FD错误原样返回。
            if (result >= 0) // 负错误不参与位运算。
                result &= (long)f->rsi | AIRI_READY_HUP | AIRI_READY_ERROR; // 关闭与错误始终报告。
            if (!result && f->rdx) { // 条件未满足且愿意有限等待。
                current->execution.io = IO_READY; // 不读写任何字节，只重查条件。
                current->execution.io_fd = (unsigned)f->rdi; // 属于此进程的稳定表项。
                current->execution.io_events = (unsigned)f->rsi; // 保存订阅条件。
                current->execution.io_deadline = airi_timer_ticks() + f->rdx; // 真实时钟截止，不靠循环次数。
                current->execution.state = USER_IO_WAIT; // 原frame已保存在进程中。
                ++io_block_count; // 这次调用确实阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=ready fd=%u", current->pid, current->execution.io_fd); // 由普通内核上下文记录。
                return schedule(REASON_IO); // 返回到同一次wait调用之后。
            }
        }
        f->rax = (uint64_t)result; // 立即就绪、纯查询、错误统一返回。
    }
    else if (f->rax == 23) // 固定注册缓冲申请，不返回内核地址。
        f->rax = (uint64_t)airi_async_buffer_alloc(current->pid); // 正handle或负错误。
    else if (f->rax == 24 || f->rax == 25) { // 注册缓冲与用户范围之间同步复制。
        uint8_t buffer[512]; // 设备不访问这个内核栈副本，之后另由请求单元持有。
        bool put = f->rax == 24; // put用户输入，get用户输出。
        long result; // 0或明确错误，字节数由已验证请求长度决定。
        if (f->rdx > sizeof(buffer)) // 完整64位长度先限制。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rsi, (size_t)f->rdx, !put)) // 输出必须可写，输入只需可读。
            result = -14; // -EFAULT，没有引擎副作用。
        else if (put) { // 输入先复制完再可能覆盖注册对象。
            copy_user(current, buffer, f->rsi, (size_t)f->rdx, false); // 不把用户VA交给资源引擎。
            result = airi_async_buffer_put(current->pid, f->rdi, buffer, (size_t)f->rdx); // BUSY拒绝，尾部清零。
        } else { // get先检查owner、handle和IDLE。
            result = airi_async_buffer_get(current->pid, f->rdi, buffer, (size_t)f->rdx); // 不收集CQ不能读取BUSY。
            if (!result && !copy_user(current, buffer, f->rsi, (size_t)f->rdx, true)) // 映射已完整验证且当前不可抢占。
                airi_panic("validated registered-buffer output changed"); // 内部错误不能静默复制。
        }
        f->rax = (uint64_t)result; // 用户自己判断资源状态。
    } else if (f->rax == 26) // 注册buffer释放不等同用户堆free。
        f->rax = (uint64_t)airi_async_buffer_free(current->pid, f->rdi); // 完整代数句柄不截断。
    else if (f->rax == 27) { // 提交描述全部复制后才接受进SQ。
        struct airi_io_submission submission; // 固定16字节无指针记录。
        if (f->rdi != AIRI_IO_READ && f->rdi != AIRI_IO_WRITE && f->rdi != AIRI_IO_FLUSH) // 完整64位操作先验证，不能截断成合法编号。
            f->rax = (uint64_t)-22ll; // 格式错误无CQ。
        else if (!copy_user(current, &submission, f->rdx, sizeof(submission), false)) // sector和cookie都由当前用户副本给出。
            f->rax = (uint64_t)-14ll; // 未接受，没有缓冲所有权变化。
        else // 已复制的数值交给资源引擎，不借用户提交结构。
            f->rax = (uint64_t)airi_async_submit(current->pid, (unsigned)f->rdi, f->rsi, &submission); // 0仅接收，I/O结果以后由CQ给。
    } else if (f->rax == 28) { // CQ收集可以非阻塞或保持原调用等待。
        struct airi_io_completion completion; // 先到内核数值副本。
        long result; // 0交出一条CQ，-11空，其他负错误。
        if (f->rsi > 1) // 只接受0不等、1阻塞。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(completion), true)) // 必须在消费CQ之前验证完整用户输出。
            result = -14; // 不丢完成、信用与buffer状态。
        else { // 当前owner的CQ与user输出身份明确。
            result = airi_async_pop(current->pid, &completion); // 可能设备很快，已有CQ直接返回。
            if (!result && !copy_user(current, &completion, f->rdi, sizeof(completion), true)) // 单线程映射稳定。
                airi_panic("validated completion output changed"); // 不能消费后隐瞒复制失败。
            if (result == -11 && f->rsi) { // 空CQ且选择阻塞，保留这同一次调用。
                current->execution.io = IO_ASYNC; // poll_waiters只收集，设备推进在启动栈。
                current->execution.io_address = f->rdi; // 用户输出页仍由进程拥有。
                current->execution.io_bytes = sizeof(completion); // 固定ABI长度，不借C局部对象。
                current->execution.state = USER_IO_WAIT; // 先保存条件和输出，再撤销运行资格。
                ++io_block_count; // 记录真实CQ等待，不强迫快设备也假装阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=cq", current->pid); // 等设备完成后原调用继续。
                return schedule(REASON_IO); // 可切其他用户或进入启动服务空闲。
            }
        }
        f->rax = (uint64_t)result; // 具体I/O错误在completion.result，不是这层0。
    } else if (f->rax == 29) { // 有界只读状态供用户与未来呈现消费者观察。
        struct airi_io_state state; // 不含内核地址或用户buffer指针。
        long result; // 当前轮次状态或负错误。
        if (!user_access(current, f->rdi, sizeof(state), true)) // 输出先验证，快照不会消耗资源。
            result = -14; // -EFAULT。
        else { // 当前PID由内核决定，用户不能查询或修改另一个owner。
            result = airi_async_state(current->pid, &state); // 包括同PID旧轮次待收集的ACTIVE数量。
            if (!result && !copy_user(current, &state, f->rdi, sizeof(state), true)) // 当前映射应保持稳定。
                airi_panic("validated I/O state output changed"); // 明确内部失败。
        }
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else if (f->rax == 30) { // 目录只返回有界文本，不暴露ELF或内核对象地址。
        char names[256]; size_t used = 0; // 当前固定目录完整小于此上限。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) { // 结果与exec使用同一真实注册表。
            size_t n=airi_strlen(executables[i].name); // 名称包含内部演示目标，用户仍可选择尝试。
            if (used+n+2>sizeof(names)) // 防止以后目录增长造成栈覆盖。
                airi_panic("program catalog exceeds bounded ABI"); // 维护者必须扩大显式上限。
            airi_memcpy(names+used,executables[i].name,n); used+=n; // 复制有效名称前缀。
            names[used++]='\n'; // 每行一个名字，界面自己呈现。
        }
        names[used]=0; // 查询返回含NUL的完整文本，不返回截断目录。
        if (f->rsi < used+1) // 小容量不能写一半。
            f->rax=(uint64_t)-28ll; // -ENOSPC。
        else if (!copy_user(current,names,f->rdi,used+1,true)) // 验证实际输出跨度。
            f->rax=(uint64_t)-14ll; // -EFAULT。
        else // 完整复制后才发布有效长度。
            f->rax=used; // 不含末尾NUL，与strlen一致。
    }
    else if (f->rax == 31) // 只归还调用者自己的动态页面，不能释放ELF或初始栈。
        f->rax = (uint64_t)release_user(f->rdi, f->rsi); // 负参数错误保持全部原映射。
    else if (f->rax == 32) { // 单调tick睡眠，复用持久等待现场而非用户忙等。
        uint64_t now = airi_timer_ticks(); // 截止以真实PIT事件计数为基准。
        if (f->rdi > 6000) // 一次最多6000tick（名义六十秒），完整64位参数先验证。
            f->rax = (uint64_t)-22ll; // -EINVAL，不改变运行状态。
        else if (!f->rdi) // 零长度是立即成功，不登记等待。
            f->rax = 0; // 和主动yield分开。
        else if (now > UINT64_MAX - f->rdi) // 不让截止计数回绕成已经过期。
            f->rax = (uint64_t)-75ll; // -EOVERFLOW。
        else { // 请求完整有效，保存条件后再退出可运行集合。
            current->execution.io = IO_SLEEP; // poll_waiters不再把它当FD操作。
            current->execution.io_fd = current->execution.io_events = 0; // 清与此次请求无关的旧数值。
            current->execution.io_address = current->execution.io_bytes = 0; // sleep不借用用户指针。
            current->execution.io_deadline = now + f->rdi; // 保留绝对截止，不在每次轮询延长。
            current->execution.state = USER_IO_WAIT; // 此前已经保存实际系统调用frame。
            ++io_block_count; // 记录真正阻塞，而非返回0的空操作。
            airi_log("[WAIT_REGISTER] pid=%u operation=sleep ticks=%llu", current->pid, (unsigned long long)f->rdi); // 普通调用栈记录，不在IRQ打印。
            return schedule(REASON_IO); // 其他用户继续；全阻塞时启动栈hlt等PIT。
        }
    }
    else if (f->rax == 33) { // 自有FS基址设置，只验证数值，不固定或分配其目标页。
        if (f->rdi >= AIRI_USER_LIMIT) // 非规范中间地址与所有内核高半区都在特权WRMSR之前拒绝。
            f->rax = (uint64_t)-22ll; // -EINVAL，旧保存值不变。
        else { // 0表示停用，其余低半区值即使尚未映射也可登记。
            current->execution.fs_base = f->rdi; // 当前硬件仍为内核0，统一返回入口才写用户值。
            f->rax = 0; // 不解引用、不读取该用户地址。
        }
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = current->execution.fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else if (f->rax == 37) { // 有符号相对定位仍由打开对象的所有者一次完成。
        if (f->rdi >= AIRI_FD_COUNT) // 完整64位编号不能截成另一个合法FD。
            f->rax = (uint64_t)-9ll; // -EBADF。
        else if (f->rdx > 2) // 原始调用也不能靠窄化whence绕过检查。
            f->rax = (uint64_t)-22ll; // -EINVAL。
        else // RSI位模式在当前二补数x86-64目标解释为有符号偏移。
            f->rax = (uint64_t)airi_fd_seek_from(&current->files, (unsigned)f->rdi, (int64_t)f->rsi, (unsigned)f->rdx); // 一次读取基准、验证并提交，旧14号绝对接口保留。
    }
    else if (f->rax == 38) { // 只提供PIT单调时钟，不伪造RTC墙钟。
        if (f->rdi != AIRI_CLOCK_MONOTONIC) // 完整参数不截成另一个合法编号。
            f->rax = (uint64_t)-22ll; // -EINVAL，输出保持。
        else { // tick仅由真实IRQ增加，同一CPU读出一次完整快照。
            uint64_t ticks = airi_timer_ticks(); // 基准是本内核PIT初始化，不含固件启动时间。
            struct airi_time_value value = {ticks / 100, (ticks % 100) * 10000000ull}; // 名义100Hz换算；纳秒单位不提高实际分辨率。
            f->rax = copy_user(current, &value, f->rsi, sizeof(value), true) ? 0 : (uint64_t)-14ll; // 完整输出先验证，坏指针不产生用户CPU故障。
        }
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
}
struct user_result { int code; unsigned ticks, switches, blocks; uint64_t fault_error; unsigned io_blocks, io_wakes, idle_halts; }; // 启动任务只取得数值，不借已回收控制块。
static int launch(const uint8_t *image, size_t bytes, const char *name, bool elf, struct user_result *result) { // 可用于启动演示或串口命令，程序失败不等于内核失败。
    if (session_active) // 当前只有一个由启动栈收拢的用户会话。
        return -16; // -EBUSY，不能覆盖已有kernel_return。
    if (next_pid == UINT32_MAX) // 不能把回绕后的0当合法新owner。
        return -75; // -EOVERFLOW，尚未分配任何资源。
    struct airi_memory_stats before, after; // 包含根、所有孩子和内核栈。
    airi_memory_snapshot(&before); // 在任何资源分配前取基准。
    struct user_process *root = unused(); // 上次会话已回收的空槽。
    if (!root) // 仍有未回收身份就明确失败。
        return -11; // -EAGAIN。
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    bool direct_shell = name && airi_strlen(name)==2 && !airi_memcmp(name,"sh",2); // init自身不读UART，只监督自己的shell孩子。
    if (direct_shell && !airi_fd_attach_input(&root->files)) // 根的新FD0必须为空。
        airi_panic("cannot attach direct shell input"); // 初始化不变量不成立。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (!airi_memory_same_usage(&before, &after)) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
        return async_error; // 应用可见普通资源失败。
    }
    root->async_owned = true; // cleanup和exit之后负责释放。
    place_frame(&root->execution); // 寄存器值进入本进程真实内核栈。
    root->execution.state = USER_RUNNABLE; // 发布首次运行资格。
    root_pid = root->pid; // 会话根身份不会被孩子exit覆盖。
    supervisor_pid = name && airi_strlen(name)==4 && !airi_memcmp(name,"init",4) ? root_pid : 0; // 只在明确run init的模式授予前台重启资格。
    input_pid = direct_shell ? root_pid : 0; // 直接shell已接入；init的孩子在成功exec时取得资格。
    if (supervisor_pid) // 普通演示的启动路径和观察标记保持。
        airi_log("[INIT_SESSION] pid=%u uart=exclusive", supervisor_pid); // PID不固定为1，身份不等于槽号。
    session_result = 0; // 不继承上次结果。
    timer_switches = 0; // 这次会话实际抢占计数。
    wait_blocks = 0; // 这次会话实际阻塞计数。
    current = root; // 内核启动门选择此根。
    io_block_count = io_wake_count = idle_halt_count = 0; // 本次I/O阻塞统计独立于wait孩子。
    session_idle = false; // 首次int129创建新活动会话。
    long code; // 根结束结果由启动现场RAX交回。
    for (;;) { // 每次返回启动栈，先区分暂时服务和整个会话结束。
        code = airi_arch_user_gate(); // 当前架构登记继续现场，返回可能暂服务或整个会话结束。
        if (!session_active) // 只有所有用户结束才离开launch。
            break; // code此时才是根用户真实退出值。
        collect_pending(); // 当前确在启动栈，仍保守跳过current槽。
        for (;;) { // 暂无可运行者时在长期内核根上服务等待条件。
            if (airi_async_work()) // 硬件完成只在长期普通服务栈推进。
                airi_async_pump(); // 一次有界服务，不在IRQ C链内运行。
            poll_waiters(); // 可复制用户结果和记录日志，不在IRQ C链里做。
            struct user_process *next = NULL; // 每次重查后重新选择。
            for (unsigned n = 1; n <= PROCESS_COUNT; ++n) // 轮转保持公平，不总挑槽0。
                if (processes[(slot(current) + n) % PROCESS_COUNT].execution.state == USER_RUNNABLE) { // 等待者可能刚被唤醒。
                    next = &processes[(slot(current) + n) % PROCESS_COUNT]; // 保存控制块，不提前切根。
                    break; // 首个可运行者获得下一次int129。
                }
            if (next) { // 服务后已有用户可继续。
                if (service_from_timer && next != current) // 只有来自真实IRQ的身份改变才计抢占。
                    ++timer_switches; // 不把用户主动阻塞算成IRQ切换。
                current = next; // int129随后正式设置TSS与目标根。
                break; // 回外层重新登记新的启动继续现场。
            }
            ++idle_halt_count; // 统计真正没有可运行用户后执行的空闲等待。
            airi_arch_idle(); // CPU真正空闲等PIT，醒后仍在启动服务栈。
        }
    }
    *result = (struct user_result){(int)code, root->execution.user_ticks, timer_switches, wait_blocks, root->execution.fault_error, io_block_count, io_wake_count, idle_halt_count}; // 回收前复制必要数值。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 当前不在任何用户内核栈上。
        if (processes[i].execution.state != USER_UNUSED) // wait已回收槽不重复处理。
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    supervisor_pid = input_pid = 0; // 有限会话结束后不能把授权留给下次运行。
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
static void run_one(const uint8_t *image, size_t bytes, const char *name, int expected, bool elf, bool require_ticks, bool require_scheduler) { // 作者启动演示使用具体预期，不控制普通命令的合法退出码。
    struct user_result result; // 会话结束只消费数值副本。
    if (launch(image, bytes, name, elf, &result) || result.code != expected || (require_ticks && result.ticks < 2) || (expected == -142 && result.fault_error != 5) || (require_scheduler && (!result.switches || !result.blocks))) // 同时验证真正执行、故障、抢占和等待。
        airi_panic("user session ended without expected semantics"); // 固定演示有误才拒绝发布READY。
    if (require_scheduler) // 实际多用户会话单独报告行为证据。
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_init(void) { // 启动只调用一次；不依赖先运行任何用户演示。
    airi_files_init(); // 为普通用户与init同样建立只读seed和空RAM目录。
    airi_log("[USER_RESOURCES_READY] slots=%u elf=static", PROCESS_COUNT); // 仅声明资源可用，不宣称已经运行服务。
}
int airi_user_boot(const char *name) { // 输入来自内核固定启动策略，不接受外部未验证指针。
    const struct executable *image = find_program(name); // 与命令入口使用同一真实映像目录。
    if (!image) // 不存在程序时尚未改变任何进程资源。
        return -2; // -ENOENT，交给启动恢复策略。
    struct user_result result; // 持续会话停止后才取得根退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 准备失败与正常返回均由launch核对回收。
    return error ? error : result.code; // 初始化失败或用户退出都不能被当作仍在服务。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    // 用户资源已由airi_user_init建立，演示不重置正式初始化状态。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    run_one(airi_files_elf_start, (size_t)((uintptr_t)airi_files_elf_end - (uintptr_t)airi_files_elf_start), "files", 0, true, false, false); // 文件内容、共享偏移与exec引用回收。
    run_one(airi_pipes_elf_start, (size_t)((uintptr_t)airi_pipes_elf_end - (uintptr_t)airi_pipes_elf_start), "pipes", 0, true, false, false); // 真实FIFO、背压、关闭和fork共享。
    struct user_result waits_result; // 数值证据不借已回收控制块。
    if (launch(airi_waits_elf_start, (size_t)((uintptr_t)airi_waits_elf_end - (uintptr_t)airi_waits_elf_start), "waits", true, &waits_result) || waits_result.code || waits_result.io_blocks < 4 || waits_result.io_wakes < 4 || !waits_result.idle_halts) // 读、写、空闲与纯计算并行超时四种实际等待均须发生。
        airi_panic("blocking I/O did not preserve waiting semantics"); // 不把用户循环重试当内核阻塞。
    airi_log("[WAIT_DEMO_OK] blocked=%u woken=%u idle_halts=%u", waits_result.io_blocks, waits_result.io_wakes, waits_result.idle_halts); // 统计来自真实状态变化。
    struct user_result async_result; // 快设备可能立即有CQ，真实阻塞次数按观察记录。
    if (launch(airi_async_elf_start, (size_t)((uintptr_t)airi_async_elf_end - (uintptr_t)airi_async_elf_start), "async", true, &async_result) || async_result.code) // 正常和受限设备由用户程序明确分支。
        airi_panic("user asynchronous lifecycle demo failed"); // 不拿submit0冒充I/O成功。
    airi_log("[ASYNC_USER_DEMO_OK] blocked=%u woken=%u switches=%u pools=reclaimed", async_result.io_blocks, async_result.io_wakes, async_result.switches); // 静态池已由launch排空并核对。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console,"programs: registered ELF images"); // 表头不硬编码另一套程序名。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) // 与exec共享真实目录，不维护第二份静态名称。
            airi_console_print(console, "%s", executables[i].name); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
        return false; // 其他命令继续原分派链。
    const char *name = command + 4; // 只在本次调用期借控制台的命令副本。
    const struct executable *image = find_program(name); // 目前目录名不是通用文件系统路径。
    if (!image) { // 错别字不影响内核继续输入。
        airi_console_print(console, "program not found: %s", name); // 给出当前名称。
        return true; // 请求已得到明确结果。
    }
#ifdef AIRI_GRAPHICAL_CONSOLE // 当前图形终端尚未接入用户TTY，不能让窗口命令陷入不可见输入等待。
    if ((bytes==6 && !airi_memcmp(name,"sh",2)) || (bytes==8 && !airi_memcmp(name,"init",4))) { // shell及其常驻监督模式都需要实际UART输入。
        airi_console_print(console,"User shell uses UART: start ./airios run, then run sh."); // 给出实际可操作入口。
        return true; // 图形桌面立即继续接收输入。
    }
#endif // 纯串口主线没有此限制。
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
        airi_console_print(console, "cannot load %s: error=%d", name, error); // 串口详细日志还给出解析原因。
    else // 程序可以合法返回非零，例如worker的23。
        airi_console_print(console, "program %s exited %d", name, result.code); // 不把应用结果当成内核停止。
    return true; // GUI是否存在都不影响服务执行。
}
```

</details>

<details>
<summary>scripts/shell_test.py 的完整终点</summary>

<!-- write: scripts/shell_test.py -->
```python
"""Drive the actual ring-3 shell through UART and observe a complete I/O path.""" # 不把宿主脚本当用户shell。
import json # 记录本轮真实结果。
from pathlib import Path # 短控制端点。
import re # 按实际PID关联等待和进展。
import socket # QEMU的UART双向字节流。
import subprocess # 只管理本测试子进程。
import tempfile # 避免UNIX路径长度上限。
import time # 单调总截止。
from boot import command, qmp_execute # 固定硬件与真实限速。
from tools import ROOT # 不依赖调用目录。
def check_shell(): # 默认串口主线，不读取frontend或字体。
    work=ROOT/'artifacts/shell-normal'; work.mkdir(parents=True,exist_ok=True) # 本测试独占目录。
    result_path=work/'result.json'; result_path.write_text('{"passed":false}\n') # 旧成功立即撤销。
    disk=work/'disk.raw' # 仅测试盘可重置，不动交互run的正式实验盘。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size!=8*1024*1024)): # 未知对象不替换。
        raise RuntimeError('Invalid shell-test disk') # 保留异常路径供检查。
    disk.unlink(missing_ok=True) # 下一command排他创建空盘。
    with tempfile.TemporaryDirectory(prefix='airios-sh-',dir='/tmp') as directory: # 本次端点随测试销毁。
        qmp=Path(directory)/'qmp.sock'; uart=Path(directory)/'uart.sock' # 控制与设备分开。
        cmd=command('normal',work); cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off' # 实际UART而非内存注入。
        cmd+=['-S','-qmp',f'unix:{qmp},server=on,wait=off'] # 连接UART后才继续CPU。
        (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n') # 固定本次QEMU参数。
        with (work/'qemu.log').open('w') as errors: # QEMU诊断单独保存。
            process=subprocess.Popen(cmd,stdout=errors,stderr=errors); wire=None; data=bytearray() # 保留真实累积字节。
            try: # 任何失败都回收自己的QEMU。
                deadline=time.monotonic()+10 # 等待端点不能无限。
                while not uart.exists(): # CPU还未开始启动。
                    if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('Shell UART unavailable') # 明确失败。
                    time.sleep(.05) # 给QEMU建立端点的机会。
                wire=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); wire.connect(str(uart)); wire.setblocking(False) # 接收端在CPU之前准备好。
                qmp_execute(qmp,'cont') # 真实继续CPU。
                def output(): # 即使失败也保留已到达的日志。
                    while True: # 取到本轮暂时没有更多字节。
                        try: chunk=wire.recv(4096) # 流不保证一行一次到齐。
                        except BlockingIOError: break # 没字节不是EOF。
                        if not chunk: break # 进程状态由wait检查。
                        data.extend(chunk) # 保持串口真实顺序。
                    (work/'serial.log').write_bytes(data) # 及时落证据。
                    return data.decode(errors='replace') # 诊断不因单个字节丢失。
                def wait(marker,offset=0): # 只接受当前请求后已完整结束的记录。
                    end=time.monotonic()+35 # 每个操作有界。
                    while time.monotonic()<end: # 不重置超时预算。
                        text=output(); tail=text[offset:] # 旧启动输出不能满足新请求。
                        for line in tail.splitlines(keepends=True): # 半行不能被当成功。
                            record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r') # 只移除已知提示符。
                            if line.endswith('\n') and (record==marker or (marker.endswith(' ') and record.startswith(marker))): return text # 明确完整记录。
                        if '[AIRI_PANIC]' in tail or process.poll() is not None: raise RuntimeError(f'Shell failed before {marker}') # 不把QEMU存活当成功。
                        time.sleep(.02) # 给真实设备与CPU推进。
                    raise RuntimeError(f'Shell timeout: {marker}; see {work}/serial.log') # 可定位失败证据。
                text=wait('[AIRI_P5_READY] ') # 完整内核启动后再进入用户shell。
                wire.sendall(b'run sh\n'); text=wait('[USER_SHELL_PROMPT]',len(text)) # 真正进入用户态shell。
                limits={'device':'airi-disk','bps':0,'bps_rd':0,'bps_wr':0,'iops':1,'iops_rd':0,'iops_wr':0} # 让物理完成晚于父等待。
                qmp_execute(qmp,'block_set_io_throttle',limits); (work/'throttle.json').write_text(json.dumps(limits)+'\n') # QMP接受后才记录。
                offset=len(text); wire.sendall(b'demo\n'); text=wait('[SHELL_COMMAND_DONE] name=demo code=0',offset) # 命令由shell解析、fork/exec/wait。
                tail=text[offset:]; match=re.search(r'\[USER_ALLOC\] pid=(\d+) ',tail) # 从实际新程序分配记录取得PID。
                assert match and '[PIPELINE_USER_OK]' in tail and '[EXEC_COMMIT]' in tail # 不用宿主复制或预期字符串替代运行。
                pid=match.group(1); progress=tail.index('[PIPELINE_OTHER_PROGRESS]'); blocked=False # 验证另一个用户在同一次CQ等待期间推进。
                for registered in re.finditer(r'\[WAIT_REGISTER\] pid='+pid+r' operation=cq\r?\n',tail): # 只匹配演示父进程。
                    wake=tail.find('[WAIT_WAKE] pid='+pid+' result=0\r\n',registered.end()) # 同PID真实结果交付。
                    blocked|=registered.start()<progress<wake # 不固定猜用户PID或时序。
                assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 实际设备请求和完成均可见。
                qmp_execute(qmp,'block_set_io_throttle',dict(limits,iops=0)) # 后续同步日志不继承慢设备测试预算。
                for request,marker in [('storage','[STORAGE_USER_OK]'),('recover','[RECOVERY_USER_OK]'),('files','[FILES_USER_OK]'),('missing','exec failed: -2'),('broken','exec failed: -8'),('hello','[ELF_USER_OK]')]: # 正常、坏名字、坏ELF后仍可运行。
                    offset=len(text); wire.sendall((request+'\n').encode()); text=wait('[SHELL_COMMAND_DONE] name='+request+' ',offset) # 完整孩子退出才开始下一条。
                    assert marker in text[offset:] # 必须来自本条命令。
                offset=len(text); wire.sendall(b'memory\n'); text=wait('[SHELL_COMMAND_DONE] name=memory code=0',offset) # 同一地址空间内归还并复用，不靠exit恢复内存。
                assert '[USER_MEMORY_REUSE_OK] rounds=128 holes=first-fit zeroed=1 invalid=unchanged' in text[offset:] # 新旧对象内容和失败原子性都由用户实际核对。
                offset=len(text); wire.sendall(b'capacity\n'); text=wait('[SHELL_COMMAND_DONE] name=capacity code=0',offset) # 大BSS、跨页用户栈、耗尽恢复与exec都来自guest。
                capacity_output=text[offset:] # 只使用本次请求后的完整输出。
                assert '[USER_CAPACITY_OK] bss=393216 stack=32768 rounds=4 full_pages=1024 exec=worker guard=pf14' in capacity_output # 用户实际逐字节核对后才发布。
                assert len(re.findall(r'\[FORK\].*copied_pages=1024',capacity_output))==4 # 四轮都触及最后一个元数据页。
                assert len(re.findall(r'\[EXEC_COMMIT\].*program=worker old_pages=1024',capacity_output))==4 # 大旧空间在成功exec提交后完整清理。
                assert 'cr2=0x6fff0000' in capacity_output # 用户栈下方保护页真正触发硬件异常。
                offset=len(text); wire.sendall(b'simd\n'); text=wait('[SHELL_COMMAND_DONE] name=simd code=0',offset) # 真实C浮点与跨抢占的活寄存器不能靠宿主模拟。
                simd_output=text[offset:] # 只核对本次命令的实际用户身份。
                assert '[SIMD_USER_OK] fork=inherited timer_window=20 x87=private xmm15=private exec=reset double=6.5' in simd_output # 父子与exec路径均由guest核对。
                assert '[SIMD_FAULT_ISOLATED] fxsave=unaligned vector=13 parent=alive' in simd_output and 'vector=13 error=0x0' in simd_output # 未对齐状态保存只结束自己的真实子身份。
                assert '[SIMD_CHILD_OK]' in simd_output and '[SIMD_EXEC_RESET_OK]' in simd_output # 子必须进入新的真实ELF检查默认状态。
                parent=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=simd ',simd_output) # shell创建的目标用户PID。
                assert parent # 不能用其他启动程序的输出代替。
                fork=re.search(r'\[FORK\] parent='+parent.group(1)+r' child=(\d+) ',simd_output) # 探针fork产生另一真实身份。
                assert fork # 本次C程序并非仅在一个进程里比较两个数组。
                for identity in (parent.group(1),fork.group(1)): # 两个用户都必须实际经过PIT中断窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',simd_output) # exec保留同一PID的用户tick统计。
                    assert ended and int(ended.group(1))>=2 # 活寄存器检查跨真实时间片，而非只有主动yield。
                offset=len(text); wire.sendall(b'tls\n'); text=wait('[SHELL_COMMAND_DONE] name=tls code=0',offset) # FS的读写必须跨真实父子抢占保持。
                tls_output=text[offset:] # 不复用启动或前一个用户程序的成功标记。
                assert '[TLS_USER_OK] fork=inherited timer_window=20 fs=private invalid=unchanged exec=reset released=pf14' in tls_output # 完整正常与释放后故障由guest检查。
                assert '[TLS_CHILD_OK]' in tls_output and '[TLS_EXEC_RESET_OK] fs_base=0 old_block=detached' in tls_output # 同一孩子成功换成新ELF。
                tls_parent=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=tls ',tls_output) # 本次命令的真实根PID。
                assert tls_parent # 不能用别的命令的fork满足条件。
                tls_fork=re.search(r'\[FORK\] parent='+tls_parent.group(1)+r' child=(\d+) ',tls_output) # 首个孩子参与不同基址的纯计算。
                assert tls_fork # 本次必须出现真实的两个可运行身份。
                for identity in (tls_parent.group(1),tls_fork.group(1)): # 二者都实际经过PIT窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',tls_output) # exec仍使用同一身份累计值。
                    assert ended and int(ended.group(1))>=2 # 主动yield或单身份内存比较不算抢占证据。
                offset=len(text); wire.sendall(b'ctls\n'); text=wait('[SHELL_COMMAND_DONE] name=ctls code=0',offset) # 编译器_Thread_local必须在main前建立，不由宿主注入值。
                ctls_output=text[offset:] # 固定本次命令的实际记录。
                assert '[CTLS_USER_OK] c11=thread_local tdata=42 tbss=73 align=64 fork=copy exec=reset invalid=unchanged' in ctls_output # 父子修改后fork、真正抢占与坏PT_TLS保持旧映像。
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'ctls-tail\n'); text=wait('[SHELL_COMMAND_DONE] name=ctls-tail code=0',offset) # 真实LLD的TLS零尾部可以超过普通LOAD内存长度。
                assert '[CTLS_TAIL_OK] initialized=4 zero_tail=257 ordinary_bss=1 independent=1' in text[offset:] # 普通BSS与本地运行块必须分别可写且互不覆盖。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged' in libc_output # 真实PIT窗口、输出权限和墙钟拒绝均由guest核对。
                assert '[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged' in libc_output # 相对定位、极值失败和真实零洞均由guest观察。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
                assert '[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked' in text[offset:] # 坏后项、跨页描述和实际前缀均由guest检查。
                offset=len(text); wire.sendall(b'heap\n'); text=wait('[SHELL_COMMAND_DONE] name=heap code=0',offset) # 对象库运行在真实用户地址空间，先耗尽原始页再验证重试。
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
                assert len(re.findall(r'\[ORPHAN_REAP\] pid=\d+ released=1',orphan_output))==24 # 不是launch收尾清空表冒充托管。
                assert len(re.findall(r'\[ORPHAN_ADOPT\].*finished=1',orphan_output))==12 # 先成为僵尸再失去父的路径实际运行。
                assert len(re.findall(r'\[ORPHAN_EXIT\]',orphan_output))==12 # 活孤儿放行后正常退出。
                offset=len(text); wire.sendall(b'programs\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 用户查询来自真实注册目录。
                assert 'demo\r\n' in text[offset:] and 'async-reset\r\n' in text[offset:] # 不是内核监控台的静态帮助。
                offset=len(text); wire.sendall(b'x'*80+b'\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 过长行必须整体丢弃。
                assert 'line too long; discarded' in text[offset:] and '[FORK]' not in text[offset:] # 不执行截断前缀。
                offset=len(text); wire.sendall(b'hellx\x7fo\r\n'); text=wait('[SHELL_COMMAND_DONE] name=hello code=0',offset) # 退格修正与CRLF真正走用户输入。
                offset=len(text); wire.sendall(b'exit\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=1 page=0',offset) # shell退出后内核监控台才返回。
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
                process.terminate() # 正常终止测试CPU，不称掉电。
                try: process.wait(timeout=5) # 清理也有期限。
                except subprocess.TimeoutExpired: process.kill(); process.wait() # 不遗留测试进程。
```

</details>

<a id="tls-tail-observe"></a>
## 先看 ELF 的两个长度，再进入 main

<!-- run -->
```sh
./airios build  # 形成真实PT_TLS和LOAD，当前仍无多执行者依赖。
PYTHONPATH=scripts python3 - <<'ELF'  # 标准输入交给Python，shell不展开引号内的脚本。
import subprocess  # 只运行实际工具，不模拟ELF内容。
from tools import tool  # 复用既有Homebrew/环境覆盖查找规则。
subprocess.run([tool('llvm-readelf', 'llvm'), '-l', 'build/normal/user/ctls-tail.elf'], check=True)  # 参数列表分别传程序头选项和实际文件，非零状态直接失败。
ELF
./airios test  # 同一解析器接受合法零尾部，并保持恶意初值、用户运行与回收验证。
```

本轮程序头中同一模板起点的两项如下，数值来自固定工具链的实际产物。TLS对齐0x10、273字节总长和普通LOAD的5字节并不矛盾；后者不是运行本地块的容量。

```text
TLS  FileSiz 0x000004  MemSiz 0x000111  R   Align 0x10
LOAD FileSiz 0x000004  MemSiz 0x000005  RW  Align 0x1000
```

交互运行./airios run，先输入run sh，再输入ctls-tail，结束后仍可ctls或heap，最后exit。应看到真实初始化和用户结果：

```text
[ELF_TLS] bytes=273 initialized=4 alignment=16 pages=1 data=0x60000000 tp=0x60000120
[CTLS_TAIL_OK] initialized=4 zero_tail=257 ordinary_bss=1 independent=1
[SHELL_COMMAND_DONE] name=ctls-tail code=0
```

这个修正覆盖静态local-exec当前子集，仍不提供动态链接、动态TLS模块或任意链接器布局。K43旧检查点继续保留当时实现，后续从K51开始使用修正后的初值与零尾部规则。Linux对照仍需在真实个人主机运行，宿主解析与guest结果不替代Linux证据。
