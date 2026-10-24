#include <assert.h> // 失败直接返回非零，不依赖文字判断。
#include <stdio.h> // 宿主输出测试结果。
#include <airi/memory.h> // 对象借用与backing缓存使用不同的资源核对。
#include <airi/heap.h> // 同一小对象算法不依赖宿主页表。
static uint8_t arena[4096] __attribute__((aligned(16))); // 宿主只提供一个受控的已拥有范围。
struct item { uint8_t *pointer; size_t bytes; uint8_t pattern; }; // 记录每个活对象的内容预期。
int main(void) { // 独立验证对象生命周期和碎片回收。
    struct airi_heap heap = {0}; // 初始没有backing。
    assert(!airi_heap_init(&heap, arena + 1, sizeof(arena) - 1)); // 未对齐起点不能偷偷接受。
    assert(!airi_heap_init(&heap, arena, 32)); // 只有块头没有payload时必须失败。
    assert(airi_heap_init(&heap, arena, sizeof(arena))); // 正常创建单块空闲堆。
    size_t initial = airi_heap_free_bytes(&heap); // 记录元数据之外的真实payload容量。
    assert(initial == sizeof(arena) - 32); // 当前块头32字节。
    assert(!airi_heap_alloc(&heap, 0) && !airi_heap_alloc(&heap, SIZE_MAX)); // 零长度和对齐溢出均失败。
    assert(!airi_heap_alloc(&heap, sizeof(arena)) && airi_heap_free_bytes(&heap) == initial); // 耗尽不修改空闲状态。
    void *a = airi_heap_alloc(&heap, 80); // 创建相邻的三块，以控制碎片位置。
    void *b = airi_heap_alloc(&heap, 128); // 后续先归还中间块。
    uint8_t *c = airi_heap_alloc(&heap, 32); // 活对象阻止跨越它合并。
    assert(a && b && c && (uintptr_t)a % 16 == 0); // 所有对象非空且对齐。
    c[31] = 0x5a; // 保留活对象尾字节，确认合并不覆盖它。
    assert(!airi_heap_free(&heap, c + 1)); // 内部指针不能作为对象起点。
    assert(airi_heap_free(&heap, b) && airi_heap_free(&heap, a)); // 两个相邻空闲块应合并。
    assert(!airi_heap_free(&heap, a)); // 重复释放不得改变状态。
    void *joined = airi_heap_alloc(&heap, 208); // 请求大于80和128，必须利用合并结果。
    assert(joined == a && c[31] == 0x5a); // first-fit复用合并块，第三对象内容不变。
    assert(airi_heap_free(&heap, joined) && airi_heap_free(&heap, c)); // 全部归还会继续与尾块合并。
    assert(airi_heap_free_bytes(&heap) == initial && airi_heap_free(&heap, NULL)); // 合并恢复最初容量，NULL为空操作。
    struct item items[16] = {0}; // 十六个同时存活对象，空槽pointer为NULL。
    uint32_t random = 0x12345678; // 固定种子使失败可以逐次复现。
    for (unsigned step = 0; step < 2000; ++step) { // 完整演示交错大小、归还和碎片重用。
        random = random * 1664525u + 1013904223u; // 无符号运算按32位回绕，生成确定序列。
        unsigned slot = (random >> 16) % 16; // 从高位选择对象槽，避免只重复低位周期。
        struct item *item = &items[slot]; // 本次只改变一个活对象或空槽。
        if (item->pointer) { // 活槽先验证再归还。
            for (size_t i = 0; i < item->bytes; ++i) // 检查整个请求范围的内容。
                assert(item->pointer[i] == item->pattern); // 其他分配和归还不能覆盖活对象。
            assert(airi_heap_free(&heap, item->pointer)); // 结束该对象生命周期。
            item->pointer = NULL; // 槽不再持有过期指针。
        } else { // 空槽尝试不同大小分配。
            item->bytes = 1 + ((random >> 8) % 240); // 覆盖不足16字节和多个对齐跨度。
            item->pointer = airi_heap_alloc(&heap, item->bytes); // 容量不足允许返回NULL。
            item->pattern = (uint8_t)(slot + 1); // 各槽使用不同内容以发现重叠。
            if (item->pointer) { // 只有取得所有权后才写入。
                assert((uintptr_t)item->pointer % 16 == 0); // 每种大小都保持16字节对齐。
                airi_memset(item->pointer, item->pattern, item->bytes); // 填满请求范围，不写对齐填充区。
            }
        }
        for (unsigned n = 0; n < 16; ++n) // 每次操作后检查所有仍活着的对象。
            if (items[n].pointer) // 空槽没有可访问内容。
                for (size_t i = 0; i < items[n].bytes; ++i) // 不只检查被选槽。
                    assert(items[n].pointer[i] == items[n].pattern); // 检测分割、合并或重复借出导致的破坏。
    }
    for (unsigned n = 0; n < 16; ++n) // 释放最后留下的活对象。
        assert(airi_heap_free(&heap, items[n].pointer)); // NULL槽也明确允许。
    assert(airi_heap_free_bytes(&heap) == initial); // 不留下隐藏块头碎片或借用泄漏。
    assert(airi_heap_init(&heap, arena, 512)); // 所有旧对象已归还，重新用同一arena前512字节建立小堆。
    uint8_t *held = airi_heap_alloc(&heap, 480); // 占满末块，使扩展走新增块头分支。
    assert(held && airi_heap_used_bytes(&heap) == 480); // 活payload不含32字节块头。
    airi_memset(held, 0x6b, 480); // 新backing接入不能移动或覆盖此对象。
    assert(airi_heap_extend(&heap, arena + 512, 512)); // 第二段虚拟上精确紧邻旧右端。
    uint8_t *added = airi_heap_alloc(&heap, 400); // 从新增空块取得另一个对象。
    assert(added && added != held && airi_heap_used_bytes(&heap) == 880); // 两个对象各自拥有payload。
    airi_memset(added, 0xa3, 400); // 后续失败与合并不能改变活内容。
    assert(!airi_heap_extend(&heap, arena + 1536, 512)); // 有洞的新范围不能进入块链。
    assert(!airi_heap_extend(&heap, arena + 1024, 32)); // 容不下块头加最小payload。
    assert(!airi_heap_extend(&heap, arena + 1024, 511)); // 长度必须保持16字节对齐。
    assert(!airi_heap_extend(&heap, arena + 1024, SIZE_MAX & ~(size_t)15)); // 巨大右端不能回绕。
    assert(heap.bytes == 1024 && airi_heap_used_bytes(&heap) == 880); // 拒绝没有悄悄发布容量。
    for (unsigned i = 0; i < 480; ++i) // 扩展前的对象所有字节仍在原地址。
        assert(held[i] == 0x6b); // 不只检查边界字节。
    for (unsigned i = 0; i < 400; ++i) // 新对象在失败扩展后也继续有效。
        assert(added[i] == 0xa3); // 不能部分覆盖已有链。
    assert(airi_heap_free(&heap, held) && airi_heap_free(&heap, added)); // 跨旧新backing边界的块可合并。
    assert(airi_heap_extend(&heap, arena + 1024, 1024)); // 空末块直接延长，不增加另一个块头。
    assert(airi_heap_free_bytes(&heap) == 2048 - 32 && !airi_heap_used_bytes(&heap)); // 只有一个初始块头，活对象为零。
    held = airi_heap_alloc(&heap, 2016); // 用跨扩展边界的大对象占满当前范围。
    assert(held && airi_heap_extend(&heap, arena + 2048, 2048)); // 已借出尾块仍走独立新块分支。
    assert(airi_heap_used_bytes(&heap) == 2016 && airi_heap_free_bytes(&heap) == 2016); // 旧对象容量不会随backing增加。
    assert(airi_heap_free(&heap, held) && airi_heap_free_bytes(&heap) == sizeof(arena) - 32); // 最后恢复一个完整空闲块。
    struct airi_memory_stats before = {.pages_free=100, .heap_bytes=65536, .heap_used=16}; // 已有一个活对象的资源快照。
    struct airi_memory_stats after = {.pages_free=84, .heap_bytes=131072, .heap_used=16}; // 十六个页仅从页池转入堆缓存。
    assert(airi_memory_same_usage(&before, &after)); // 空闲缓存增长不会冒充对象泄漏。
    --after.pages_free; // 另有一个不属于堆缓存的页面没有归还。
    assert(!airi_memory_same_usage(&before, &after)); // 真实页泄漏仍须被拒绝。
    ++after.pages_free; // 还原页面数，单独改变活对象容量。
    after.heap_used = 32; // 新活payload未归还，不能被缓存容量掩盖。
    assert(!airi_memory_same_usage(&before, &after)); // 对象泄漏与backing缓存分开判断。
    puts("PASS heap: alignment, overflow, exhaustion, invalid free, coalescing, 2000 lifetime steps, contiguous extension, cache vs live usage"); // 全部实际断言通过。
    return 0; // 正常退出宿主。
}
