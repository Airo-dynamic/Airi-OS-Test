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

本章每组第一块只用于定位旧文本，只敲第二块替换。未列出的部分保持。

<!-- replace: kernel/process/elf.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

<a id="tls-tail-faults"></a>
## 合法零尾部与缺失初值分开观察

host的tls_cases先保留原73字节模板，再把普通LOAD memsz改为4，四字节初值仍完整，应该通过。接着把LOAD的filesz和memsz都缩为1，LOAD本身仍合法，但不足以提供TLS的四字节初值，必须拒绝并保持旧view。两种输入只差实际可提供的文件内容，不能统一按TLS总长拒绝。

再构造纯零模板，把起点放在四字节LOAD的右端，解析通过且不读取该位置；将起点移到完全无LOAD的0x500000，则继续拒绝。最后恢复fixture，让已有超限、错误对齐、重复TLS、页预算和程序头顺序检查独立运行。下面在 tests/unit/elf_test.c 原 tls_cases 中接入这组相邻输入。

<!-- replace: tests/unit/elf_test.c -->
<!-- before -->
```c
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 合法TLS不能继续沿旧策略拒绝。
    assert(view.pages == 3 && view.tls.file_bytes == 4 && view.tls.memory_bytes == 73); // LOAD预算和运行副本长度分别记录。
    assert(view.tls.pointer_offset == 80 && view.tls.data_offset == 0 && view.tls.pages == 1); // 73按16向上舍入为80，TCB仍在同页。
    put(208, 1, 8); put(216, 1, 8); put(224, 1, 8); // 只有一字节且只要求1字节对齐。
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 小于TCB对齐的模板仍合法。
    assert(view.tls.pointer_offset == 16 && view.tls.data_offset == 15 && view.tls.pages == 1); // 变量位于TP-1，不能从块首错误初始化。
```
<!-- after -->
```c
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
```

<a id="tls-tail-integration"></a>
## 接入同一份初始程序目录

Makefile末尾新增ctls-tail.elf，复用原始USER_COMMON，不链接posix.o或线程库。images.S以只读incbin包入实际ELF；user.c只增加范围声明与ctls-tail目录项。进程、执行者、FS恢复和用户TLS初始化代码保持K50状态。

shell_test在ctls完成以后执行ctls-tail，只有完整独立标记与实际退出0都出现才继续。新增名字后的完整目录仍容得下原256字节预算，不需要改变shell。下面直接接入构建、包装、目录和原命令序列，未列出的内容保持。

在 `Makefile` 末尾追加以下内容，既有部分保留。

<!-- append: Makefile -->
```make

# 单独观察TLS零尾部与较小普通BSS，不需要先实现多个执行者。
$(OUT)/user/ctls-tail.elf: $(USER_COMMON) $(OUT)/user/ctls-tail.o labs/airios/program.ld
# 文件初值来自tdata，tbss运行空间不由普通LOAD长度替代。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始映像等当前配置的完整链接结果。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls-tail.elf
# 只读incbin路径在包装对象中消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_TAIL_ELF='"$(OUT)/user/ctls-tail.elf"'
```

在 `labs/airios/images.S` 末尾追加以下内容，既有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* TLS零尾部观察仍经真实静态ELF装载。 */
.global airi_ctls_tail_elf_start, airi_ctls_tail_elf_end /* 输入范围供真实初始程序目录使用。 */
airi_ctls_tail_elf_start: /* 普通BSS小于tbss的最小程序。 */
.incbin AIRI_CTLS_TAIL_ELF /* 当前配置真正链接的模板与LOAD描述。 */
airi_ctls_tail_elf_end: /* 完整文件的排他右端。 */
```

声明ctls-tail的实际ELF边界。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
extern const uint8_t airi_tls_big_elf_start[], airi_tls_big_elf_end[]; // 真正含超限PT_TLS的错误ELF。
extern const uint8_t airi_tls_elf_start[], airi_tls_elf_end[]; // FS本地数据与切换探针。
extern const uint8_t airi_tls_reset_elf_start[], airi_tls_reset_elf_end[]; // 成功exec清基址观察。
```
<!-- after -->
```c
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
extern const uint8_t airi_ctls_tail_elf_start[], airi_ctls_tail_elf_end[]; // TLS零尾部大于普通LOAD的真实链接器布局。
extern const uint8_t airi_tls_big_elf_start[], airi_tls_big_elf_end[]; // 真正含超限PT_TLS的错误ELF。
extern const uint8_t airi_tls_elf_start[], airi_tls_elf_end[]; // FS本地数据与切换探针。
extern const uint8_t airi_tls_reset_elf_start[], airi_tls_reset_elf_end[]; // 成功exec清基址观察。
```

在目录里接入ctls-tail，装载和执行状态代码保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
    {"ctls-page", airi_ctls_page_elf_start, airi_ctls_page_elf_end}, // 两页运行块的真实边界。
    {"tls-too-big", airi_tls_big_elf_start, airi_tls_big_elf_end}, // 真实装载器必须拒绝，不进入用户main。
    {"tls", airi_tls_elf_start, airi_tls_elf_end}, // 每个用户身份独立FS基址。
    {"tls-reset", airi_tls_reset_elf_start, airi_tls_reset_elf_end}, // 新ELF不沿用旧FS。
```
<!-- after -->
```c
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
    {"ctls-page", airi_ctls_page_elf_start, airi_ctls_page_elf_end}, // 两页运行块的真实边界。
    {"ctls-tail", airi_ctls_tail_elf_start, airi_ctls_tail_elf_end}, // 普通BSS与TLS零尾部各自分配。
    {"tls-too-big", airi_tls_big_elf_start, airi_tls_big_elf_end}, // 真实装载器必须拒绝，不进入用户main。
    {"tls", airi_tls_elf_start, airi_tls_elf_end}, // 每个用户身份独立FS基址。
    {"tls-reset", airi_tls_reset_elf_start, airi_tls_reset_elf_end}, // 新ELF不沿用旧FS。
```

在ctls命令完成后运行ctls-tail，并从这次独立命令核对零尾部与普通BSS保持。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged' in libc_output # 真实PIT窗口、输出权限和墙钟拒绝均由guest核对。
```
<!-- after -->
```python
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'ctls-tail\n'); text=wait('[SHELL_COMMAND_DONE] name=ctls-tail code=0',offset) # 真实LLD的TLS零尾部可以超过普通LOAD内存长度。
                assert '[CTLS_TAIL_OK] initialized=4 zero_tail=257 ordinary_bss=1 independent=1' in text[offset:] # 普通BSS与本地运行块必须分别可写且互不覆盖。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged' in libc_output # 真实PIT窗口、输出权限和墙钟拒绝均由guest核对。
```

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
