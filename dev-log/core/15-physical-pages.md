# K15 · 把可用 RAM 交给明确的页面所有者

从 K14 继续。异常已经能定位到指令，接下来需要一块不会覆盖加载器、内核栈或显示设备的 RAM。启动图回答“哪些范围原本可用”，分配器还必须回答“这些可用范围中哪些页已经交给调用者”。本节新建物理页池，借出两页，在其中一页的真实直接映射上写入并读回，归还后重新分配，确认所有权与数量恢复。

保留 K14 的异常入口与启动适配。新建 pages.h、pages.c、memory.c 和一个主机边界观察，再替换 Makefile 与 main.c；中途先不编译。图形、动态内存分配和进程都不是本节前置。

<a id="page-ownership"></a>
## 地图描述和分配状态是两件事

我们固定每页 4096 字节。物理地址除以 4096 得到页帧号，页帧号乘 4096 回到页起点。一个 usable 区域不一定恰好对齐：起点要向上取整，右端要向下取整，只有完全落在区域里的页才能借出。例如 `[0x10003,0x14000)` 只包含 0x11000、0x12000、0x13000 三个完整页；不能从 0x10000 开始覆盖前面的保留字节。

两张位图分别记录 managed 和 allocated。一位对应一页，页号除 8 定位字节、余 8 定位其中的位。managed=0 的页面从未属于这个页池；managed=1 且 allocated=0 才是空闲；两位都是 1 才是已借出。释放只清 allocated，不能把 reserved 或内核页改造成 usable。页零永久保留，让返回值 0 可以明确表示耗尽。

首版位图只覆盖低 1 GiB，约占 64 KiB 内核静态空间；当前固定机器只有 256 MiB。高于上限的 RAM 保留不用，并在日志输出容量限制。这样不需要先有堆来存页池元数据，也没有假装支持任意大机器。元数据属于内核映像，内核物理范围再次被排除；BOOT_RECLAIM、ACPI、帧缓冲等范围本轮一律不回收。

**新建 `include/airi/pages.h`。** API 返回物理页起点，不返回可直接解引用的宿主或内核虚拟指针。

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
## 借出之前先改变状态

初始化先让所有位都为零，再从有效地图中仅标记完整 usable 页，最后排除页零与任何和内核相交的页面，建立 total/free。boot_validate 已经检查区域排序、重叠和加法溢出；分配器仍在计算前调用它，拒绝损坏输入。起点已经限制在 1 GiB 内，向上对齐加 4095 不会发生整数回绕。

分配从 hint 开始扫描，最多一圈。只有确定 managed 且未 allocated 才先标记借出、减少 free，最后返回地址；否则同一页面可能在交出后仍被当成空闲。归还先检查对齐、范围、managed 和 allocated，全部成立才改变统计，重复归还不会让 free 超过 total。

当前单 CPU 且内核不可抢占，位图操作没有锁。以后中断或并发调用分配器时必须重新保护这段状态，不能从“现在能运行”推断多核安全。接口也不能识别持有旧地址的过期调用者；所有者必须先结束映射和其他借用，再归还页面，归还后不能继续读写旧指针。

**新建 `kernel/mm/pages.c`。** `1u << (index % 8)` 把一个 1 移到所选位；`~mask` 反转位后用于只清这一位。

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
## 物理页怎样成为 C 能访问的地址

C 指针在当前开启分页的 CPU 上表示虚拟地址。比如分配器返回物理 0x1000，并不意味着可以直接解引用 `(uint64_t *)0x1000`。Limine 提供的 HHDM 让物理地址 p 在 `direct_map_offset+p` 处有一个内核虚拟别名，CPU 再通过当前页表把它翻译回 p。这个加法依赖启动合同已经存在的映射，不是指针转换自动创建映射。

memory.c 持有页池，保留第一次分配前的 free，借出两个不同页，向第一个页的别名写入标记并读回，再验证正常释放、重复释放拒绝、未对齐释放拒绝和低地址复用。`volatile` 使演示的读写确实发生，不是多线程同步原语。页面返回时内容不清零；当前调用者必须自行初始化，后续交给用户进程前还必须清除旧所有者数据。

**新建 `kernel/mm/memory.c`。** 页面归还后日志只记录其数值，不再解引用 mapped。

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
    *mapped = 0x4149524950414745ull; // 真正写入刚借出的 RAM，volatile 保留设备级观察所需的访问。
    if (*mapped != 0x4149524950414745ull || pages.free != before - 2) // 验证内容和所有权统计是同一次分配结果。
        airi_panic("physical page write/accounting failed"); // 不能只凭非零地址宣称可用。
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
## 用小地图观察耗尽与非法归还

真实 guest 验证 HHDM 和 RAM；主机观察则把地图缩到十个可借出页，可以完整借尽并检查所有地址，避免只验证一次成功。该程序直接调用同一 pages.c，却绝不把返回的物理地址当成 Mac 指针。它还故意把内核写在 usable 区里，确认额外保留生效；最后让地图重叠，确认初始化拒绝它。

**新建 `tests/unit/pages_test.c`。** 宿主 assert 与 sanitizer 只用于作者的边界验证，不进入 guest 或成为学习者习题。

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

**完整替换 `Makefile`。** 默认串口源码增加两个 mm 文件，host-test 增加同一页算法的 ASan/UBSan 观察。

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
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c
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
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 预处理器从自有include、固定协议头third_party和前端include取得声明。
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
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
$(OUT)/%.o: %.c
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S
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
```

**完整替换 `kernel/main.c`。** 内存初始化位于启动校验和异常表之后、命令循环之前。

<!-- write: kernel/main.c -->
```c
#include <airi/pages.h> // 在稳定内存地图上建立物理页所有权。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。

static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。

#ifndef AIRI_DEMO_MESSAGE  // 允许修改实验在编译时替换输出文本。
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 普通配置默认输出的学习标记文字。
#endif  // 结束默认演示文字的条件定义
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 引导适配交入稳定启动描述；_Noreturn 声明内核不返回加载器。
    const char *error = airi_boot_validate(info);  // 校验必需启动字段，NULL 表示通过，字符串给出失败原因。
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    airi_trap_demo(); // 正常启动验证带与不带硬件错误码的返回路径。
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
    airi_memory_init(info); // 只借出完整 usable 页，并在真实 RAM 演示写入与归还。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 宽高转为与 %llu 相同的参数类型。
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    /* Written AFTER all drawing, so tests can take a completed screenshot. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
            airi_arch_relax();  // 提示当前架构正在忙等；这不是睡眠或调度。
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
```

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

正常串口应在 READY 前新增 `[PAGES_READY]` 和 `[PAGES_DEMO_OK]`。前者输出 managed 与 free，相等说明尚未借出；后者输出 first、second、reused，first 与 second 不同，reused 与 first 相同，free 恢复。地址来自本轮启动图，不固定写死为某一页。主机观察输出 `PASS pages: inward rounding, ownership, exhaustion, reuse, invalid maps`。

作者的故障操作已经写在主机程序中：耗尽后再申请得到 0，释放 `values[0]+1` 被拒绝，重复释放也被拒绝，重叠地图不能初始化。它们都不使宿主访问无效物理内存。guest 若实际写入或统计不符合预期，则给出具体 PANIC，不发布 READY。

调试可在 `airi_page_alloc` 设置断点，观察 `pool->free` 先减一再返回；在 memory.c 写标记后，使用 GDB 的 `x/gx` 读取 `info->direct_map_offset+first`。必须读取虚拟别名，不能把 GDB 默认内存读取误认为物理总线访问。对应 Linux 的区别是：malloc/mmap 得到的通常是虚拟地址范围，RSS 反映驻留页；本节直接给内核调用者借出物理页，尚无按需缺页分配。P2 的 Linux 对照将用 mmap 与实际触碰把这两件事分开观察。

下一节用这些页面存页表，再装入 CR3，真正接管地址翻译；在此之前，HHDM 和所有已有内核映射仍由 Limine 建立。
