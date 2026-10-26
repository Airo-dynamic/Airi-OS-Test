# K30 · 提交设备请求与收集完成分成两个动作

K29 能在父等管道时运行孩子，因为等待请求已从 C 调用链移进进程状态。磁盘 perform 却仍包含“发布后等到底”的循环：在它返回前，上层即使想安排其他工作，也拿不到控制权。先把设备请求的交出与收取拆开，才能把等待位置上移。先拆开驱动边界：begin只接收请求，poll只检查一次；原同步read/write/flush用这两个函数组合，旧日志行为保持。下一节的队列才有机会在begin之后安排其他工作，而不是把一个同步函数包在“异步”名字里。

<a id="block-begin-poll"></a>
## 0表示接收，还是0表示完成，取决于当前接口

仍以读同一个扇区为例。调用者先 begin，之后可以离开驱动做别的事，稍后再 poll；因此类型和截止时间必须由驱动保存到两次调用之间。begin 验证操作、扇区、RO 与 FLUSH 能力，确认没有busy后，复制写数据、填写描述符、发布avail并通知设备。返回0只说明这次请求被接收；active_type与request_deadline留在驱动静态状态中，不能随着C局部变量返回而丢失。即使设备已经写完used，只要还没由poll收集，busy仍真，第二次begin返回−16，不覆盖旧请求。

再次进入驱动时，poll 面对的是 begin 留下的那条请求，不能根据新的参数猜它是哪种操作。poll 返回 1 表示仍在途，0表示本次完成，负数表示错误。它不hlt、不循环，也不在每次检查时重设截止。观察used后继续核对链首id、长度与status，再复制读数据并清busy。读输出可以是NULL表示明确丢弃，但DMA静态区仍要完成收集后才能复用。内核里的单个可信消费者拥有这组接口，用户不能直接调用poll抢别人的完成。

两秒deadline在首次begin固定，反复poll不能无限延长。超时仍reset并隔离静态DMA，后续begin返回−5，不自动重试。设备完成很快时，第一次poll也可以立即返回0；不能为演示效果硬要求一定观察到pending。

**更新块接口。** 同步0/负错误和poll的1待完成分别声明，pending表示未收集的所有权。

<!-- write: include/airi/block.h -->
```c
#ifndef AIRI_BLOCK_H // 通用块接口不暴露PCI端口和VirtIO布局。
#define AIRI_BLOCK_H // 包含保护。
#include <airi/boot.h> // 当前启动提供内核连续物理映像位置。
int airi_block_init(const AiriBootInfo *info); // 固定单队列，失败返回负错误。
bool airi_block_readonly(void); // 给调用者明确只读事实，不能靠写失败猜测。
uint64_t airi_block_sectors(void); // 以512字节扇区计，不等同设备逻辑块大小配置。
int airi_block_read(uint64_t sector, void *buffer); // 一次精确一个512字节扇区，0成功。
int airi_block_write(uint64_t sector, const void *buffer); // 同步复制并等待设备完成。
int airi_block_flush(void); // 请求后端同步，不宣称真实硬件掉电已实测。
int airi_block_begin(unsigned operation, uint64_t sector, const void *write_data); // 0读/1写/4同步；0表示已接收，不表示完成。
int airi_block_poll(void *read_data); // 1仍在途、0完成、负错误；读输出可NULL表示丢弃，静态 DMA 区继续保留，收集完成前不得复用。
bool airi_block_pending(void); // 已接收但未收集完成，含设备已完成却未poll的状态。
void airi_block_demo(void); // 只操作项目独立实验盘末扇区。
#endif // 结束保护。
```

<a id="block-sync-adapter"></a>
## 同步适配只收集自己成功提交的请求

日志目前仍需要“这一次扇区操作结束以后才继续下一步”，不应因拆接口而改变提交顺序。同步 perform 因而只是新接口的一个消费者。perform 保存 IF 后调用 begin；只有它成功才反复poll，遇1由PIT唤醒后继续，其他结果直接返回。若设备已经属于另一请求，begin的−16直接交给调用者，不得继续poll把别人的数据收走。临时开IRQ时仍在CS0，不抢占内核，旧同步日志顺序不变。

启动演示先begin读同一扇区，确认pending，并立刻第二次begin得到−16；接着poll收集并比较原字节。BLOCK_NONBLOCK_OK证明的是独占提交和真实完成收集，不声称已经让用户等待或实现多在途队列。只读设备同样可验证读；故意漏发布idx的故障配置先超时隔离，不再运行这个正常演示。

**更新驱动的三个确定窗口。** 描述符格式与发布顺序沿K25，只把提交、一次收集和同步等待分开。

先增加跨 begin/poll 保存的 active_type 与 deadline。它们随静态驱动存在，不随某次 C 调用返回而消失。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
static uint64_t request_physical, capacity; // 设备使用guest物理地址，不理解内核高VA。
```
<!-- after -->
```c
static uint64_t request_physical, capacity; // 设备使用guest物理地址，不理解内核高VA。
static uint32_t active_type; // 当前在途操作只属于成功begin的调用者。
static uint64_t request_deadline; // 截止跨begin/poll调用保存，不能每次poll重置。
```

把原 perform 整体替换成 begin、pending、poll 和新的同步 perform。先完整给出新边界，再保留旧 read/write/flush 的调用形式；下面的块一直到公开 read 包装以前。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
static int perform(uint32_t type, uint64_t sector, void *buffer) { // 单队列且最多一个在途请求。
    if (!ready) // 之前超时或未初始化。
        return -5; // -EIO，不覆盖可能仍被设备使用的旧DMA。
    if (busy) // 不支持重入和多个调用者覆盖同一静态请求。
        return -16; // -EBUSY。
    if (type != 4 && sector >= capacity) // 在设备读取任何描述符前拒绝越界扇区。
        return -22; // -EINVAL。
    if (type == 1 && readonly) // 设备只读不产生写DMA。
        return -30; // -EROFS。
    if (type == 4 && !can_flush) // 不把缺少FLUSH伪装同步成功。
        return -95; // -EOPNOTSUPP。
    uint64_t flags = airi_irq_save(); // 队列发布部分对本CPU不可打断。
    busy = true; // 先取得静态请求所有权。
    request.type = type; // IN=0、OUT=1、FLUSH=4。
    request.reserved = 0; // 协议保留字段写零。
    request.sector = type == 4 ? 0 : sector; // FLUSH不指定数据扇区。
    request.status = 0xff; // 设备必须覆盖状态，不能误用上次成功。
    if (type == 1) // 写请求先复制到内核长期DMA缓冲。
        airi_memcpy(request.data, buffer, 512); // 上层栈和用户页都不交给设备。
    descriptors[0] = (struct descriptor){request_physical, 16, 1, type == 4 ? 2 : 1}; // 设备只读请求头，NEXT指向数据或状态。
    descriptors[1] = (struct descriptor){request_physical + 16, 512, (uint16_t)(1u | (type == 0 ? 2u : 0u)), 2}; // read数据由设备写，write数据由设备读。
    descriptors[2] = (struct descriptor){request_physical + offsetof(struct block_request, status), 1, 2, 0}; // 状态字节始终由设备写。
    available[2 + available_index % queue_size] = 0; // 环项写链首编号0，不是请求指针。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 先让设备看到整个链与avail槽。
#ifndef AIRI_TEST_BLOCK_TIMEOUT // 故障镜像故意漏发布idx，模拟驱动发布遗漏而非声称设备硬件挂死。
    available[1] = ++available_index; // 只有idx前进才交出链的所有权。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 发布idx在端口通知之前对设备可见。
    airi_out16(io + 16, 0); // 通知队列0有请求。
#endif // 故障分支仍有真实时间上限。
    uint64_t deadline = airi_timer_ticks() + 200; // 真实100Hz IRQ，最多约两秒等待。
    while (used[1] == used_index) { // 设备先写数据/状态，最后发布used.idx。
        if (airi_timer_ticks() >= deadline) { // 不是按忙循环次数估算时间。
            int result = stop(-110); // -ETIMEDOUT，静态DMA区隔离且不再复用。
            airi_irq_restore(flags); // 保留调用前的IF策略。
            return result; // 上层得到具体错误。
        }
        __asm__ volatile("sti; hlt; cli" ::: "memory"); // 暂开IRQ等待PIT唤醒；CS0不会被用户调度器抢占。
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 观察idx之后再读取设备完成内容。
    unsigned next_index = used[1]; // 读取这次设备序号。
    struct used_element completion = used_ring[used_index % queue_size]; // 同一槽的完整id与length。
    if ((uint16_t)(next_index - used_index) != 1 || completion.id != 0 || completion.length < 1 || completion.length > (type == 0 ? 513u : 1u)) { // 单在途只能有一条本链完成，状态至少一字节。
        int result = stop(-5); // 无法确认DMA生命周期就停止，不复用缓冲。
        airi_irq_restore(flags); // 恢复原调用环境。
        return result; // 坏完成不是成功。
    }
    if (request.status == 0 && type == 0 && completion.length != 513) { // 成功读必须写完512数据和一个状态字节。
        int result = stop(-5); // 不把短设备完成当完整扇区。
        airi_irq_restore(flags); // 恢复原IF。
        return result; // 上层缓冲尚未写入。
    }
    ++used_index; // 本次已消费，不再重复处理同一个槽。
    int result = request.status == 0 ? 0 : request.status == 2 ? -95 : -5; // 设备IOERR与UNSUPP都明确交回。
    if (!result && type == 0) // 只有成功读才能把设备数据交给上层。
        airi_memcpy(buffer, request.data, 512); // 复制结束后设备DMA缓冲才可供下次覆盖。
    (void)airi_in8(io + 19); // 清可能遗留的legacy ISR状态，完成仍以used环判断。
    busy = false; // 已确认完成，可以重用同一静态请求。
    airi_irq_restore(flags); // 系统调用进入时IF0，普通内核演示进入时IF1。
    return result; // 0表示设备完成，不自动代表应用事务持久化。
}
```
<!-- after -->
```c
int airi_block_begin(unsigned type, uint64_t sector, const void *write_data) { // 只提交，不在这个调用中等设备完成。
    if ((type != 0 && type != 1 && type != 4) || (type == 1 && !write_data)) // 内核接口也验证操作与输入，用户不能直接调用。
        return -22; // -EINVAL。
    if (!ready) // 之前超时或未初始化。
        return -5; // -EIO，不覆盖可能仍被设备使用的旧DMA。
    if (busy) // 不支持重入和多个调用者覆盖同一静态请求。
        return -16; // -EBUSY。
    if (type != 4 && sector >= capacity) // 在设备读取任何描述符前拒绝越界扇区。
        return -22; // -EINVAL。
    if (type == 1 && readonly) // 设备只读不产生写DMA。
        return -30; // -EROFS。
    if (type == 4 && !can_flush) // 不把缺少FLUSH伪装同步成功。
        return -95; // -EOPNOTSUPP。
    uint64_t flags = airi_irq_save(); // 队列发布部分对本CPU不可打断。
    active_type = type; // 类型与静态DMA所有权一起保存到后续poll。
    busy = true; // 先取得静态请求所有权。
    request.type = type; // IN=0、OUT=1、FLUSH=4。
    request.reserved = 0; // 协议保留字段写零。
    request.sector = type == 4 ? 0 : sector; // FLUSH不指定数据扇区。
    request.status = 0xff; // 设备必须覆盖状态，不能误用上次成功。
    if (type == 1) // 写请求先复制到内核长期DMA缓冲。
        airi_memcpy(request.data, write_data, 512); // 上层栈和用户页都不交给设备。
    descriptors[0] = (struct descriptor){request_physical, 16, 1, type == 4 ? 2 : 1}; // 设备只读请求头，NEXT指向数据或状态。
    descriptors[1] = (struct descriptor){request_physical + 16, 512, (uint16_t)(1u | (type == 0 ? 2u : 0u)), 2}; // read数据由设备写，write数据由设备读。
    descriptors[2] = (struct descriptor){request_physical + offsetof(struct block_request, status), 1, 2, 0}; // 状态字节始终由设备写。
    available[2 + available_index % queue_size] = 0; // 环项写链首编号0，不是请求指针。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 先让设备看到整个链与avail槽。
#ifndef AIRI_TEST_BLOCK_TIMEOUT // 故障镜像故意漏发布idx，模拟驱动发布遗漏而非声称设备硬件挂死。
    available[1] = ++available_index; // 只有idx前进才交出链的所有权。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 发布idx在端口通知之前对设备可见。
    airi_out16(io + 16, 0); // 通知队列0有请求。
#endif // 故障分支仍有真实时间上限。
    request_deadline = airi_timer_ticks() + 200; // 一次提交只建立一个真实截止，不因多次poll延长。
    airi_irq_restore(flags); // 返回到原调用者的IF策略，不在begin中睡眠。
    return 0; // 仅表示已接受，busy直到poll收集或错误隔离才释放。
}
bool airi_block_pending(void) { // 仅查询驱动所有权状态。
    return busy; // 设备可能已写used，未收集仍属于在途请求。
}
int airi_block_poll(void *buffer) { // 只检查一次，不hlt，不要求用户指针。
    if (!busy) // 没有本驱动接收的请求可收集。
        return -22; // -EINVAL，不能读上次残留status当新完成。
    uint64_t flags = airi_irq_save(); // 完成收集和busy撤销作为同一次内核操作。
    if (used[1] == used_index) { // 设备尚未发布新完成。
        int result = airi_timer_ticks() >= request_deadline ? stop(-110) : 1; // 真超时才reset并隔离，1明确仍在途。
        airi_irq_restore(flags); // 无完成也不改变原IF。
        return result; // 调用者可去做别的工作，之后再poll。
    }
    unsigned type = active_type; // 不从新调用参数猜测旧请求类型。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 观察idx之后再读取设备完成内容。
    unsigned next_index = used[1]; // 读取这次设备序号。
    struct used_element completion = used_ring[used_index % queue_size]; // 同一槽的完整id与length。
    if ((uint16_t)(next_index - used_index) != 1 || completion.id != 0 || completion.length < 1 || completion.length > (type == 0 ? 513u : 1u)) { // 单在途只能有一条本链完成，状态至少一字节。
        int result = stop(-5); // 无法确认DMA生命周期就停止，不复用缓冲。
        airi_irq_restore(flags); // 恢复原调用环境。
        return result; // 坏完成不是成功。
    }
    if (request.status == 0 && type == 0 && completion.length != 513) { // 成功读必须写完512数据和一个状态字节。
        int result = stop(-5); // 不把短设备完成当完整扇区。
        airi_irq_restore(flags); // 恢复原IF。
        return result; // 上层缓冲尚未写入。
    }
    ++used_index; // 本次已消费，不再重复处理同一个槽。
    int result = request.status == 0 ? 0 : request.status == 2 ? -95 : -5; // 设备IOERR与UNSUPP都明确交回。
    if (!result && type == 0 && buffer) // 只有成功读才能把设备数据交给上层。
        airi_memcpy(buffer, request.data, 512); // 复制结束后设备DMA缓冲才可供下次覆盖。
    (void)airi_in8(io + 19); // 清可能遗留的legacy ISR状态，完成仍以used环判断。
    busy = false; // 已确认完成，可以重用同一静态请求。
    airi_irq_restore(flags); // 系统调用进入时IF0，普通内核演示进入时IF1。
    return result; // 收集完成后才能重用静态DMA，0不是应用事务持久化。
}
static int perform(unsigned type, uint64_t sector, void *buffer) { // 原同步接口组合begin与poll，语义保持。
    uint64_t flags = airi_irq_save(); // 等待过程仍临时开IRQ，但不抢占CS0。
    int result = airi_block_begin(type, sector, buffer); // busy或参数错误不会偷取别人的完成。
    if (!result) // 只有本次提交成功才可以收集。
        while ((result = airi_block_poll(buffer)) == 1) // 1是驱动待完成状态，不是成功字节数。
            __asm__ volatile("sti; hlt; cli" ::: "memory"); // 用PIT唤醒后重查，截止由首次begin固定。
    airi_irq_restore(flags); // 系统调用IF0与普通内核IF1都按原样恢复。
    return result; // 0或负错误，旧read/write/flush契约不变。
}
```

在 demo 保存原扇区以后加入 begin/重叠拒绝/poll 观察，再接回原写读恢复路径。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
#endif // 正常路径还需验证真正读写。
    if (error) // 初始读取必须完成。
        airi_panic("cannot read block demo sector"); // 不用未初始化before覆盖磁盘。
    for (unsigned i = 0; i < sizeof(pattern); ++i) // 不用全零数据掩盖没执行的写。
        pattern[i] = (uint8_t)(i * 37u + 19u); // 可复现非零模式。
    if (readonly) { // 只读盘允许读取，但写必须拒绝。
```
<!-- after -->
```c
#endif // 正常路径还需验证真正读写。
    if (error) // 初始读取必须完成。
        airi_panic("cannot read block demo sector"); // 不用未初始化before覆盖磁盘。
    if (airi_block_begin(0, sector, NULL) || !airi_block_pending() || airi_block_begin(0, sector, NULL) != -16) // 接收后即使设备已快完成，也不可覆盖未收集请求。
        airi_panic("nonblocking block submission ownership failed"); // 不把第二次提交覆盖旧DMA。
    int polled; // 首次poll可能已完成，不能伪造必须观测到pending。
    while ((polled = airi_block_poll(after)) == 1) // 每次只看一次，等待策略留在调用者。
        __asm__ volatile("sti; hlt" ::: "memory"); // 此启动演示等待真实PIT，不改变begin的非等待语义。
    if (polled || airi_block_pending() || airi_memcmp(before, after, 512)) // 完成身份和原内容都要正确。
        airi_panic("nonblocking block completion failed"); // 不能只看busy清了。
    airi_log("[BLOCK_NONBLOCK_OK] accepted=1 overlap=-16 collected=1"); // 未收集前所有权独占，实际数据读回。
    for (unsigned i = 0; i < sizeof(pattern); ++i) // 不用全零数据掩盖没执行的写。
        pattern[i] = (uint8_t)(i * 37u + 19u); // 可复现非零模式。
    if (readonly) { // 只读盘允许读取，但写必须拒绝。
```

<a id="block-log-interlock"></a>
## 日志事务不能偷用一个已在途的块请求

现在 begin 能返回而 busy 仍在，日志调用就可能遇到别人已经交出的请求。若让 journal 先开始、内部再碰到 −16，它会按 I/O 失败把事务标成不确定；实际上本次日志尚无副作用。service 在进入 journal_append 或 journal_read 前检查 pending，若设备属于别的请求就返回−16。这个拒绝发生在日志算法之前，不会把一个尚未开始的事务误置为poisoned。count只读缓存数值，不触发设备I/O。当前单核不可抢占内核使一次同步日志操作内部不会插进另一用户提交；未来异步完成只在普通服务边界处理，IRQ仍不直接发新日志事务。

**更新正式日志服务。** 保存明确忙状态，旧记录与错误语义不变。

<!-- write: kernel/storage/service.c -->
```c
#include <airi/storage.h> // 进程和控制台只消费数值结果。
#include <airi/journal.h> // 完整提交与恢复协议。
#include <airi/block.h> // 固定单设备后端。
#include <airi/kernel.h> // 实际恢复记录。
static struct airi_journal store; // 内核拥有，跨用户进程退出保留。
static bool available; // 初始化失败或设备受限不能伪造空日志。
static int read_sector(void *context, uint64_t sector, void *data) { // 窄同步适配不包含用户指针。
    (void)context; // 当前只有一块实验盘。
    return airi_block_read(sector, data); // 真实VirtIO完成。
}
static int write_sector(void *context, uint64_t sector, const void *data) { // 算法写入独立正式区域。
    (void)context; // 单设备无需额外状态。
    return airi_block_write(sector, data); // 错误保持原样。
}
static int flush(void *context) { // 不以普通write完成代替同步。
    (void)context; // 接口形状与日志后端一致。
    return airi_block_flush(); // 虚拟后端FLUSH。
}
int airi_storage_init(void) { // 正常启动只扫描，不偷偷新增记录。
    available = false; // 旧内存状态不可当恢复结果。
    if (!airi_block_sectors()) { // 超时隔离后不能继续读盘。
        airi_log("[STORAGE_SKIPPED] block_unavailable=1"); // 明确本阶段未提供磁盘服务。
        return 0; // 故障配置可继续非存储命令。
    }
    struct airi_journal_io io = {NULL, read_sector, write_sector, flush, airi_block_sectors()}; // 函数与静态设备在整个启动期有效。
    int error = airi_journal_scan(&store, &io, 16, 64); // 正式区占16–143，避开演示扇区与盘末尾。
    if (error) // 真实I/O错误不能当新空盘。
        return error; // main决定报告并停止。
    available = true; // 完整恢复后才接受请求。
    airi_log("[STORAGE_RECOVERED] count=%u truncated=%u readonly=%u", store.count, store.truncated, airi_block_readonly()); // 字段来自扫描与真实设备特征。
    return 0; // 空日志与已有前缀都合法。
}
long airi_storage_count(void) { // 用户取得数值，不持有store指针。
    return !available ? -19 : store.poisoned ? -5 : (long)store.count; // 缺设备、不确定与可用分开。
}
long airi_storage_append(const void *data, size_t bytes) { // 当前同步副本不跨调用保存。
    if (!available) // 超时设备不能继续发布请求。
        return -19; // -ENODEV。
    if (airi_block_readonly()) // 只读状态查询不破坏已恢复前缀。
        return -30; // -EROFS，不把正常只读拒绝标成不确定写。
    if (airi_block_pending()) // 异步适配接入后，不能把别人的在途请求当本次日志I/O。
        return -16; // -EBUSY，未进入日志所以不会错误置poisoned。
    return airi_journal_append(&store, data, bytes); // 数据、提交和同步均完成后才成功。
}
long airi_storage_read(unsigned index, void *data, size_t capacity) { // 单条记录不是RAM文件偏移接口。
    if (airi_block_pending()) // 读日志同样不能收集其他调用者的完成。
        return -16; // -EBUSY，原日志状态保持。
    return available ? airi_journal_read(&store, index, data, capacity) : -19; // 返回实际长度或明确错误。
}
```

**更新启动检查。** 可用读设备必须真实运行begin/重叠拒绝/poll，隔离设备不得继续请求。

非隔离盘还需 begin、重叠拒绝、poll 的真实完成标记。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
```
<!-- after -->
```python
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        if case != 'block-timeout': # 隔离盘不再允许新提交。
                            assert '[BLOCK_NONBLOCK_OK]' in text # begin接受、重叠拒绝和poll收集已运行。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
```


```sh
./airios test # 原同步、管道、故障与跨启动恢复继续通过。
./airios run # 看BLOCK_NONBLOCK_OK，之后仍可run storage和run waits。
```

正常结果为accepted=1、overlap=−16、collected=1；读回内容和原扇区一致。故障block-timeout仍在真实PIT截止后返回−110，下一次读得到−5，不因接口拆分延长等待或重用DMA。原BLOCK_DEMO_OK、JOURNAL_DEMO_OK、WAIT_DEMO_OK和双启动恢复仍要成立。

下一节在这个可信单设备消费者上加有限SQ/CQ与稳定请求身份。设备描述符id、软件请求cookie、注册缓冲句柄将分别有自己的含义；不能直接用返回顺序猜出完成属于谁。
