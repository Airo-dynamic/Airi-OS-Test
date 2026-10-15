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
