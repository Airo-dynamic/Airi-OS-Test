# K15 · 把可用 RAM 交给明确的页面所有者

异常路径已能报告坏指令，现在要取得一块可写且不会覆盖内核、加载器或显示设备的 RAM。启动地图告诉我们范围最初的用途，却不会在借出一页后自动改写；分配器必须另记运行状态，防止把同一页同时交给两个调用者。

本章借出两页，在第一项的真实 HHDM 别名写入并读回，再归还、复用，最后核对数量恢复。保留启动与异常设施，新增 pages 接口、算法、内存入口和主机边界观察，再接 Make/main；全部写齐后构建，不先要求堆、进程或图形。

<a id="page-ownership"></a>

## 一页能不能分配，需要两种状态共同判断

固定页大小 4096。地址除以 4096 得页号，页号乘回得到起点。usable 范围可能没对齐，例如 `[0x10003,0x14000)` 只有 0x11000、0x12000、0x13000 三个完整页；起点向上、右端向下取整，不能覆盖 0x10000 后的保留部分。

地图归类以后，managed 位图标哪些页属于池，allocated 再标其中哪些已借出。每位一页，页号除 8 找字节、余 8 找位。managed=0 从未归池；1/0 可分配，1/1 已有所有者。归还只清 allocated，不把 reserved 变 usable，所以两张位图不能合并。页零始终保留，使返回 0 明确表示失败。

首版只覆盖低 1 GiB，两张位图约占 64 KiB 静态内核空间；固定机器为 256 MiB，高地址暂不使用并记录限制，无需先建堆存元数据。位图本身属于内核映像，初始化再排除相交内核页；BOOT_RECLAIM、ACPI、帧缓冲本轮仍保留。

**新建 `include/airi/pages.h`。** 返回物理页起点，尚不是可直接读取的 C 指针。

<!-- write: include/airi/pages.h -->
```c
#ifndef AIRI_PAGES_H // 物理页接口只声明一次。
#define AIRI_PAGES_H // 包含保护标记。
#include <airi/boot.h> // 分配器只消费稳定启动地图，不包含 Limine 协议。
#define AIRI_PAGE_SIZE 4096ull // 一个页帧的字节数。
#define AIRI_PHYSICAL_LIMIT (1024ull * 1024 * 1024) // 首版只管理低 1 GiB，固定目标内存为 256 MiB。
#define AIRI_PAGE_COUNT (AIRI_PHYSICAL_LIMIT / AIRI_PAGE_SIZE) // 位图可表示的页帧总数。
struct airi_page_pool { // 两张位图区分“允许管理”和“本分配器当前已借出”。
    uint8_t managed[AIRI_PAGE_COUNT / 8]; // 一位对应一个完整可用页帧。
    uint8_t allocated[AIRI_PAGE_COUNT / 8]; // 只对 managed 中的页面记借出状态。
    uint32_t total, free, hint; // 管理总页数、空闲页数和下一次扫描起点。
};
bool airi_pages_init(struct airi_page_pool *pool, const AiriBootInfo *info); // 初始化只接收有效地图，不回收加载器区域。
uint64_t airi_page_alloc(struct airi_page_pool *pool); // 返回物理页起点，耗尽返回 0；物理页 0 永不借出。
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical); // 对齐、归属和借出状态均有效才释放。
void airi_memory_init(const AiriBootInfo *info); // 在真实 guest 上建立页池并演示分配、写入和回收。
#endif // 结束包含保护。
```

<a id="page-allocate"></a>

## 分配先占位，再把地址交给调用者

初始化先验证地图，位图清零，只纳入完整 usable 页，再排除零页和相交内核页，形成 total/free。验证已经限制排序、重叠和加法；本实现还在计算前调用它，不借坏地图建池。向上对齐时起点已小于 1 GiB，加 4095 不会回绕。

从 hint 最多扫描一圈，找到 managed 且未 allocated，先置借出位、减 free，再返回地址。例如借第一项以后地图仍写 usable，但 allocated 已为 1，下一次就跳过它。若先返回而不记状态，这项区别便不存在。

归还反过来先核对对齐、上限、managed 和 allocated，全部成立才清位加 free，重复归还不增加数量。`1u << (index%8)` 选择位，取反后清位保留同字节其他页。

当前单 CPU、不可抢占内核串行调用，没有锁；若以后 IRQ 或并发也调用，需重新保护状态。地址不带代数，不能识别旧持有者，所有者必须先结束其他借用/映射再归还，之后停止使用旧指针。

**新建 `kernel/mm/pages.c`：**

<!-- write: kernel/mm/pages.c -->
```c
#include <airi/base.h> // 使用自己的字节清零实现。
#include <airi/pages.h> // 物理页状态与固定管理范围。
static bool bit(const uint8_t *map, uint64_t index) { // 读取某个页帧对应的一位。
    return (map[index / 8] & (1u << (index % 8))) != 0; // 商定位字节，余数定位字节中的位。
}
static void change(uint8_t *map, uint64_t index, bool value) { // 只修改当前页帧的一位。
    uint8_t mask = (uint8_t)(1u << (index % 8)); // 页号的低三位选择一个位掩码。
    if (value) // 借出或纳入管理时置位。
        map[index / 8] |= mask; // 按位或保留同字节其他页面的状态。
    else // 释放或排除范围时清位。
        map[index / 8] &= (uint8_t)~mask; // 取反掩码只清目标位。
}
```

bit/change 已能读写一个页号对应的状态。初始化先把完整 usable 页的 managed 位置一，再排除页零和相交内核页；allocated 仍全零，所以 total 统计完成时，free 与它相等。

**继续追加页池初始化：**

<!-- append: kernel/mm/pages.c -->
```c
bool airi_pages_init(struct airi_page_pool *pool, const AiriBootInfo *info) { // 调用者持有 pool 的全部寿命。
    if (!pool || airi_boot_validate(info)) // 无效地图不参与任何地址计算。
        return false; // 不把损坏地图解释成可分配物理内存。
    airi_memset(pool, 0, sizeof(*pool)); // 初始所有页都不可管理且未借出。
    for (uint32_t i = 0; i < info->region_count; ++i) { // 按已验证的有序非重叠地图检查区域。
        const struct airi_memory_region *r = &info->regions[i]; // 只借用描述，不修改 BootInfo。
        if (r->kind != AIRI_MEMORY_USABLE || r->base >= AIRI_PHYSICAL_LIMIT) // ACPI、内核、设备和加载器区域均保持保留。
            continue; // 高于管理上限的 RAM 当前也不纳入页池。
        uint64_t end = r->base + r->length; // boot_validate 已排除加法溢出。
        if (end > AIRI_PHYSICAL_LIMIT) // 固定容量不能被高地址范围越界索引。
            end = AIRI_PHYSICAL_LIMIT; // 截到位图可表示的半开右端。
        uint64_t first = (r->base + AIRI_PAGE_SIZE - 1) / AIRI_PAGE_SIZE; // 起点向上取整，放弃不完整的第一页。
        uint64_t last = end / AIRI_PAGE_SIZE; // 右端向下取整，放弃不完整的最后一页。
        for (uint64_t page = first; page < last; ++page) // 只标记完全落入可用区的页。
            change(pool->managed, page, true); // 此时还没有任何页面借出。
    }
    uint64_t kernel_end = info->kernel_physical_base + info->kernel_size; // 已验证的内核物理半开区间。
    for (uint64_t page = 0; page < AIRI_PAGE_COUNT; ++page) { // 一次扫描建立准确统计，同时额外保护内核范围。
        uint64_t start = page * AIRI_PAGE_SIZE; // 页号转换回物理起点。
        if (!page || (start < kernel_end && start + AIRI_PAGE_SIZE > info->kernel_physical_base)) // 页零作失败哨兵；任何与内核相交的页都保留。
            change(pool->managed, page, false); // 即使错误地把内核标成 usable，也不交给调用者。
        if (bit(pool->managed, page)) // 只有实际可管理页进入统计。
            ++pool->total; // 初始全部可管理页都还空闲。
    }
    pool->free = pool->total; // total 是容量，free 随借出和归还变化。
    pool->hint = 1; // 从页一开始查找，页零永不分配。
    return pool->total != 0; // 无完整可用页时明确拒绝启动后续机制。
}
```

页池现在保存可管理范围。借用稍后主机夹具演算：0x1000 是页号 1，managed[0] 的位 1 为一，allocated 的同一位为零，hint 也从 1 开始。第一次分配先将 allocated[0] 的位 1 置一、free 减一，再交出 0x1000；即使地图仍写 usable，第二次也会跳过它。guest 的真实起点仍由本次启动图决定。

**继续追加借页操作：**

<!-- append: kernel/mm/pages.c -->
```c
uint64_t airi_page_alloc(struct airi_page_pool *pool) { // 单核、不可抢占内核下串行修改位图。
    if (!pool || !pool->free) // 空池或耗尽不扫描、不修改统计。
        return 0; // 0 明确表示未取得任何页面。
    for (uint64_t n = 0; n < AIRI_PAGE_COUNT; ++n) { // 最多一圈，避免失配状态产生无限循环。
        uint64_t page = (pool->hint + n) % AIRI_PAGE_COUNT; // 越过上界后从零继续，位图索引总在范围内。
        if (!bit(pool->managed, page) || bit(pool->allocated, page)) // 保留或已借出的页面都不能再次借出。
            continue; // 检查下一个候选页。
        change(pool->allocated, page, true); // 在交出地址前先记录所有权。
        --pool->free; // 一次成功恰好减少一个空闲页。
        pool->hint = (uint32_t)((page + 1) % AIRI_PAGE_COUNT); // 下一次优先从当前页之后扫描。
        return page * AIRI_PAGE_SIZE; // 返回物理地址，不是可直接解引用的 C 指针。
    }
    return 0; // 统计失配也不得返回未拥有的页面。
}
```

返回的物理页由调用者持有，分配器已不再把它当空闲。归还必须重新确认它属于本池、对齐而且仍标作 allocated，成功只清借出位；managed 继续为一。重复归还因此停在检查处，free 不会第二次增加。

**最后追加归还操作：**

<!-- append: kernel/mm/pages.c -->
```c
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical) { // 调用者必须先结束所有借用与映射，再归还页。
    if (!pool || !physical || physical >= AIRI_PHYSICAL_LIMIT || physical % AIRI_PAGE_SIZE) // 拒绝空哨兵、越界和页内地址。
        return false; // 不让无效释放改变位图。
    uint64_t page = physical / AIRI_PAGE_SIZE; // 对齐地址换成唯一页号。
    if (!bit(pool->managed, page) || !bit(pool->allocated, page)) // 拒绝保留页和重复释放。
        return false; // 不增加空闲统计，也不让别人的页面进入空闲池。
    change(pool->allocated, page, false); // 页从已借出回到可借出。
    ++pool->free; // 恰好归还一个页面。
    if (page < pool->hint) // 下次可以尽早复用刚归还的低地址页。
        pool->hint = (uint32_t)page; // 缩短下一次扫描，不代表页面内容已清零。
    return true; // 状态改变后才报告归还成功。
}
```

<a id="page-direct-map"></a>

## 已拥有物理页，还要通过有效映射访问它

分配器只交出物理编号。假设 p=0x1000，当前 C 仍使用虚拟地址，不能直接把 p 当指针。Limine 已将它映到 direct_map_offset+p，CPU 再按现有页表访问同一页；加法利用已有映射，不创建映射。

memory.c 静态持有池，先记 free，再借两个不同页。通过第一项的 HHDM 写标记、读回并核对 free 恰少 2，再检查正常归还、重复/未对齐拒绝和低地址复用。volatile 保留实际读写，不是多线程同步。归还不清内容，下一所有者要自行初始化，交用户前还必须清掉旧数据。

演示结束后日志只保留地址数值，不继续读取已经归还的 mapped。

**新建 `kernel/mm/memory.c`：**

<!-- write: kernel/mm/memory.c -->
```c
#include <airi/pages.h> // 物理页分配与启动信息。
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
    if (!airi_pages_init(&pages, info)) // 没有可管理页就不能继续建立内存服务。
        airi_panic("physical page pool has no usable pages"); // 停止，而不是假装分配成功。
    airi_log("[PAGES_READY] page_bytes=4096 capacity_bytes=%llu managed=%u free=%u", // 明确固定上限和实际管理数量。
             (unsigned long long)AIRI_PHYSICAL_LIMIT, pages.total, pages.free); // 内存地图中的非 usable 页面不在统计中。
    uint32_t before = pages.free; // 保存演示前空闲数量，最后必须恢复。
    uint64_t first = airi_page_alloc(&pages); // 取得第一个物理页，0 是失败。
    uint64_t second = airi_page_alloc(&pages); // 第二个页面必须独立借出。
    if (!first || !second || first == second) // 耗尽和重复借出都不允许继续写入。
        airi_panic("physical page allocation failed"); // 停止潜在的内存破坏。
    volatile uint64_t *mapped = (volatile uint64_t *)(uintptr_t)(info->direct_map_offset + first); // HHDM 将物理页转换为当前可访问的虚拟别名。
    *mapped = 0x4149524950414745ull; // 真正写入刚借出的 RAM，volatile 保留本次普通 RAM 写读，不提供并发同步。
    if (*mapped != 0x4149524950414745ull || pages.free != before - 2) // 验证内容和所有权统计是同一次分配结果。
        airi_panic("physical page write/accounting failed"); // 不能只凭非零地址宣称可用。
```

两页已独立借出，mapped 只借第一项的 HHDM 别名完成写读，free 已比 before 少二。下面开始归还与复用，不再解引用 mapped；again 取得的页可以与 first 数值相同，却代表新一次借用。最终两项都还回，计数恢复才报告演示完成。

**继续追加归还、拒绝与复用，闭合 memory_init：**

<!-- append: kernel/mm/memory.c -->
```c
    if (!airi_page_free(&pages, first) || airi_page_free(&pages, first) || airi_page_free(&pages, second + 1)) // 验证正常归还、重复释放拒绝和未对齐释放拒绝。
        airi_panic("physical page release validation failed"); // 失败释放不应改变 second 的所有权。
    uint64_t again = airi_page_alloc(&pages); // 应复用刚归还的低地址页。
    if (again != first || !airi_page_free(&pages, again) || !airi_page_free(&pages, second) || pages.free != before) // 所有演示借用在结束前归还。
        airi_panic("physical page reuse/accounting failed"); // 任何泄漏都阻止后续阶段。
    airi_log("[PAGES_DEMO_OK] first=0x%llx second=0x%llx reused=0x%llx free=%u", // 日志给出本轮真实分配地址。
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
}
```

<a id="page-boundaries"></a>

## 把小池借尽，才能看见耗尽后的状态

guest 写读验证真实 RAM 与 HHDM；主机用同一 pages.c，却只把物理地址当实验编号，不解引用它们。小夹具总共十页，逐项借尽可以核对唯一性，第十一次得 0，再依次归还验证总量恢复。

夹具还把内核故意放在 usable 内，检查额外排除；不对齐区域检验向内取整，最后改成重叠地图应拒绝初始化。assert/sanitizer 都在主机执行，不进入 guest，也不成为读者习题。

**新建 `tests/unit/pages_test.c`：**

<!-- write: tests/unit/pages_test.c -->
```c
#include <assert.h> // 失败状态交给 Make，不依赖输出文字猜测成功。
#include <stdio.h> // 主机检查可使用宿主输出，guest 实现不链接它。
#include <airi/pages.h> // 使用同一页分配器实现。
static struct airi_page_pool pool; // 大位图不放进小型测试栈。
int main(void) { // 主机只验证地址与所有权算法，不把物理地址当宿主指针。
    AiriBootInfo info = {.magic=AIRI_BOOT_MAGIC, .version=AIRI_BOOT_VERSION, .size=sizeof(info), .direct_map_offset=0xffff800000000000ull, .kernel_physical_base=0x2000, .kernel_virtual_base=0xffffffff80000000ull, .kernel_size=4096, .region_count=4}; // 构造可验证的启动描述。
    info.regions[0] = (struct airi_memory_region){0, 0x9000, AIRI_MEMORY_USABLE, 0}; // 内含页零和故意误标的内核页，均须额外排除。
    info.regions[1] = (struct airi_memory_region){0x9000, 0x7000, AIRI_MEMORY_RESERVED, 0}; // 中间保留范围不能分配。
    info.regions[2] = (struct airi_memory_region){0x10003, 0x3ffd, AIRI_MEMORY_USABLE, 0}; // 向内取整后只有 0x11000、0x12000、0x13000 三页。
    info.regions[3] = (struct airi_memory_region){AIRI_PHYSICAL_LIMIT, 4096, AIRI_MEMORY_USABLE, 0}; // 超过固定管理上限的页不纳入位图。
    assert(airi_pages_init(&pool, &info)); // 正常地图应形成非空页池。
    assert(pool.total == 10 && pool.free == 10); // 第一段七页，第三段三页。
    uint64_t values[10]; // 保留本轮全部借出地址，以核对唯一性和最终回收。
    for (unsigned i = 0; i < 10; ++i) { // 借出直到耗尽，而不是只试一次成功。
        values[i] = airi_page_alloc(&pool); // 调用真实分配器，不用替身。
        assert(values[i] && values[i] != 0x2000 && values[i] % 4096 == 0); // 页零、内核和未对齐地址均不能返回。
        assert(values[i] < 0x9000 || (values[i] >= 0x11000 && values[i] < 0x14000)); // 不得借出保留或不完整页面。
        for (unsigned j = 0; j < i; ++j) // 与所有仍借出的页比较。
            assert(values[i] != values[j]); // 不能同时把同一页交给两个所有者。
    }
    assert(pool.free == 0 && airi_page_alloc(&pool) == 0); // 耗尽必须返回失败且保持统计。
    assert(!airi_page_free(&pool, 0) && !airi_page_free(&pool, 0x2000)); // 页零和内核不能伪装成有效释放。
    assert(!airi_page_free(&pool, 0x9000) && !airi_page_free(&pool, AIRI_PHYSICAL_LIMIT)); // 保留范围和上界均被拒绝。
    assert(!airi_page_free(&pool, values[0] + 1) && pool.free == 0); // 未对齐释放不改变原页状态。
    for (unsigned i = 0; i < 10; ++i) // 归还每一个真实借出的页。
        assert(airi_page_free(&pool, values[i])); // 正确释放必须成功。
    assert(pool.free == pool.total && !airi_page_free(&pool, values[0])); // 全部收回后重复释放仍失败。
    info.regions[1].base = 0x8000; // 作者完整演示重叠地图的拒绝。
    assert(!airi_pages_init(&pool, &info)); // 无效地图不能重新建立可分配范围。
    puts("PASS pages: inward rounding, ownership, exhaustion, reuse, invalid maps"); // 实际全部断言通过后输出。
    return 0; // 正常返回主机。
}
```

将 pages 与 memory 加入默认内核，将 pages_test 与同一算法加入 host-test。前者随后实际写 RAM，后者只核对范围与所有权，两种观察各保留自己的证据。

**局部更新 `Makefile` 的源码清单与 host-test：**

在文件中依次找到以下旧窗口，分别用对应新文本完整替换；窗口以外保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续替换下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```
<!-- after -->
```make
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 独立编译同一物理页算法，主机不解引用 guest 地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/pages_test.c \
	    kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/pages_test
# 验证耗尽、保留范围、重复释放和舍入边界。
	./build/tests/pages_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```

启动信息验证、异常设施就绪以后才调用内存初始化，失败可报告具体原因；成功演示结束才进入命令循环。页面元数据属于长期内核状态，不放在即将返回的入口局部数组中。

**在 `kernel/main.c` 加入 pages 声明，并在会话初始化前调用 memory_init：**

在文件中依次找到以下旧窗口，分别用对应新文本完整替换；窗口以外保留。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
```
<!-- after -->
```c
#include <airi/pages.h> // 在稳定内存地图上建立物理页所有权。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
```

继续替换下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
```
<!-- after -->
```c
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 只借出完整 usable 页，并在真实 RAM 演示写入与归还。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
```

从统一入口运行主机边界与当前 guest 回归，按本次实际日志核对地址和计数。

<!-- run -->
```sh
./airios build  # 交叉编译并形成同一配置的镜像。
PYTHONPATH=scripts python3 - <<'PYPAGES'  # 原样交给Python，不由Shell展开。
import subprocess  # 外部命令按参数列表执行。
from tools import make_command  # 继续使用同一工具选择规则。
from boot import check  # 继续等待本轮guest标记并回收QEMU。
subprocess.run(make_command(target='host-test'), check=True)  # 主机穷尽小地图和非法释放。
check('normal')  # guest实际写入、读回、归还并复用RAM。
PYPAGES
```

READY 前应新增 PAGES_READY 与 PAGES_DEMO_OK。前者在首次借出前 managed/free 相等；后者 first≠second、reused=first，最终 free 恢复。地址由本次启动图决定，不固定为某个常量。主机应打印 inward rounding、ownership、exhaustion、reuse、invalid maps 的 PASS。

耗尽、`values[0]+1`、重复归还和重叠地图都已在观察中完整实现，不让主机访问无效物理范围。guest 写读或计数不符则 PANIC，不发布 READY。

调试 airi_page_alloc 时先看 free 减少，再到 memory.c 写后用 x/gx 读 direct_map_offset+first；GDB 默认读取虚拟地址，不能当物理总线探针。Linux malloc/mmap 通常交出虚拟范围，RSS 表示驻留页，而这里直接借物理页，还没有按需缺页。

下一章将把借出的页用作页表、装入 CR3，接管翻译；当前访问仍依赖 Limine 建立的 HHDM 与内核映射。
