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
