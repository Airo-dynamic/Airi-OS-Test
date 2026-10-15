# K31 · 提交队列可以满，完成还没收集也不能提前释放缓冲

从K30继续。本节先把异步资源管理写成独立算法，再用真实VirtIO后端执行四条请求：写、FLUSH、读回、越界错误。用户系统调用接下一节。这样可以在引入用户地址和进程退出之前，单独看清SQ容量、CQ信用、请求身份和晚完成的生命周期。

<a id="async-three-identities"></a>
## 槽号、句柄和cookie分别指认谁

设备只认识K25的描述符链首id0。本节软件请求单元另存cookie，它由提交者选择，用来把完成关联回原请求，不按返回位置猜身份。注册buffer使用带代数的handle：低字节为池索引加1，高位是单调generation；释放后复用同一槽会产生新handle，旧整数不能命中新对象。

每个owner还有epoch，表示同一编号当前这一轮资源。将来PID在exec前后不变，但旧映像的请求不能写入新映像的buffer或CQ，因此reset先确认能取得新epoch，再撤销旧轮次。完成同时核对owner、epoch和handle。owner不是内部进程指针，关闭和槽复用不会让请求留下悬空控制块地址。

共享io_abi只有固定宽度数值：submission含sector与cookie，completion含cookie、buffer、result、operation和零reserved。result才是I/O结果：读写成功512，FLUSH成功0，失败为负；submit返回0仅接收，pump返回0仅表示服务函数没有内部失败，pop返回0仅表示复制出一条完成。这些返回不能互相替代。

**新建 `include/airi/io_abi.h`。** 先固定不含指针的自有记录，下一节用户编译才接入它，不称Linux ABI。

<!-- write: include/airi/io_abi.h -->
```c
#ifndef AIRI_IO_ABI_H // 未来用户与内核共享的自有固定宽度记录，不含内部指针。
#define AIRI_IO_ABI_H // 包含保护。
#include <stdint.h> // 明确字段宽度，避免宿主long大小影响布局。
#define AIRI_IO_READ 0u // 固定512字节块读取。
#define AIRI_IO_WRITE 1u // 固定512字节块写入。
#define AIRI_IO_FLUSH 4u // 设备同步，无数据缓冲。
struct airi_io_submission { uint64_t sector, cookie; }; // 用户描述只含数值，缓冲句柄单独传入。
struct airi_io_completion { // 结果归属于明确cookie与buffer，不按完成顺序猜身份。
    uint64_t cookie, buffer; // 用户请求身份与注册缓冲句柄。
    int64_t result; // 读写成功512，FLUSH成功0，错误负数。
    uint32_t operation, reserved; // 操作类型与必须为零的保留字段。
};
_Static_assert(sizeof(struct airi_io_submission) == 16, "submission ABI size"); // 读用户结构前固定字节数。
_Static_assert(sizeof(struct airi_io_completion) == 32, "completion ABI size"); // CQ输出不能随编译器结构变化。
#endif // 结束保护。
```

<a id="async-credit"></a>
## 接受请求时就为完成预留位置

引擎固定8个owner、32个512字节注册buffer、每owner最多4个buffer，全局4个排队/ACTIVE请求，每owner4个CQ信用。SQ用四个请求单元保存完整副本，按sequence选择最早排队项；CQ使用head/count环。它不是共享内存环，也不宣称io_uring的零拷贝提交路径。

提交先检查操作、handle、重复cookie、owner信用与全局空槽；都满足后才复制写数据、把buffer设BUSY并加outstanding。失败−11表示根本没有接受，buffer仍IDLE。设备完成后请求单元可回收，但outstanding不减，CQ与buffer继续占用；只有pop收集后才归还信用并把buffer解锁。因此SQ已空也可能因为CQ未收集而不能提交，不能无限积累无人处理的完成。

同一buffer从提交到CQ收集始终BUSY，put/get/free都返回−16，包括硬件已经完成的时段。用户必须先取得这条完成，才可查看或重用buffer。不同owner不能拿外来handle访问内容。cookie在当前owner未收集请求中必须唯一，收集后可复用。

<a id="async-late-owner"></a>
## owner退出后，设备还可能在做什么

请求单元自己持有512字节副本，写提交时从注册buffer复制；真实DMA仍由K30驱动的静态请求持有。关闭owner可以立即取消QUEUED、撤销注册对象与CQ，但ACTIVE请求单元必须保留到后端poll结束。即使原owner已经不存在，后端仍有一个稳定输出地址，收集后按epoch判断无主并丢弃完成，再回收该单元。

reset与直接close分别覆盖exec和退出的需要。同一编号新epoch可以拿到复用槽的新handle，旧完成不能被交给新CQ，也不能覆盖新内容。这里没有让设备直接DMA到用户页，所以进程页面回收和设备目标寿命可以分开；以后若实现用户页直达DMA，就必须固定页面并显式等待设备结束，不能照搬当前撤销策略。

pump每次最多收集一个ACTIVE，再尝试最早QUEUED；一旦begin成功就返回，不等设备。最多处理四个立即错误，避免坏输入使内核服务无界循环。越界扇区是已接受请求的I/O错误，通过CQ−22返回且不进入后端；不支持的操作格式则在submit阶段立即−22，没有CQ。后端暂忙−16保留SQ项，设备错误也形成CQ。内部信用或handle不变量破坏才让引擎faulted并要求内核停止。

**新建 `include/airi/async.h`。** 这些内部池结构只供算法、内核和主机模型使用，用户只看io_abi数值。

<!-- write: include/airi/async.h -->
```c
#ifndef AIRI_ASYNC_H // 有界异步算法与进程、PCI适配分开。
#define AIRI_ASYNC_H // 包含保护。
#include <airi/base.h> // 复制、长度和固定宽度。
#include <airi/io_abi.h> // 自有提交/完成数值记录。
#include <stdbool.h> // 引擎状态。
#define AIRI_AIO_OWNERS 8u // 与当前进程预算相同，算法本身只认识owner编号。
#define AIRI_AIO_BUFFERS 32u // 每owner最多四个注册缓冲。
#define AIRI_AIO_REQUESTS 4u // 全局排队与在途合计四个。
#define AIRI_AIO_CQ 4u // 每owner最多四个未收集完成，提交先预留信用。
enum airi_aio_buffer_state { AIRI_AIO_BUFFER_FREE, AIRI_AIO_BUFFER_IDLE, AIRI_AIO_BUFFER_BUSY }; // BUSY直到CQ被收集。
enum airi_aio_request_state { AIRI_AIO_REQUEST_FREE, AIRI_AIO_QUEUED, AIRI_AIO_ACTIVE }; // ACTIVE不能在owner退出时提前重用。
struct airi_aio_backend { // 可信单消费者后端，begin不等待、poll只检查一次。
    void *context; // 后端自有状态寿命覆盖引擎。
    int (*begin)(void *, unsigned, uint64_t, const void *); // 0接收，-16忙，其他负错误形成CQ。
    int (*poll)(void *, void *); // 1在途、0完成、负错误。
};
struct airi_aio_owner { // 每次打开/重置拥有新的epoch。
    unsigned id, outstanding, head, count; // 身份、预留信用和CQ环位置。
    uint64_t epoch; // 同一PID exec后仍能区分旧映像请求。
    struct airi_io_completion completions[AIRI_AIO_CQ]; // 有界结果，不借用户内存。
};
struct airi_aio_buffer { // 设备不直接DMA到这里，队列单元另持有请求副本。
    unsigned owner; // ID0保留为无owner。
    uint64_t epoch, handle; // owner轮次与代数句柄同时校验。
    enum airi_aio_buffer_state state; // free/idle/busy决定修改与释放权限。
    uint8_t data[512]; // 固定注册缓冲，重用先清零。
};
struct airi_aio_request { // 即使owner退出，ACTIVE单元仍由引擎持有。
    enum airi_aio_request_state state; // 队列槽的真实生命周期。
    unsigned owner, operation; // 不借进程控制块指针。
    uint64_t epoch, handle, cookie, sequence, sector; // 身份与FIFO提交顺序分别保存。
    uint8_t data[512]; // 写提交时复制，读完成时暂存；不借注册缓冲地址。
};
struct airi_aio_engine { // 固定静态对象，不能复制后当两份资源管理器。
    struct airi_aio_backend backend; // 单设备接收/完成接口。
    struct airi_aio_owner owners[AIRI_AIO_OWNERS]; // owner生命周期独立于表槽复用。
    struct airi_aio_buffer buffers[AIRI_AIO_BUFFERS]; // 全局注册池。
    struct airi_aio_request requests[AIRI_AIO_REQUESTS]; // SQ与单在途共享容量。
    int active; // -1无硬件在途，否则指向ACTIVE槽。
    uint64_t epoch, generation, sequence, base, sectors; // 代数不倒退，用户块范围与正式日志隔离。
    bool faulted; // 内部不变量失配后不能继续静默运行。
};
int airi_aio_init(struct airi_aio_engine *engine, const struct airi_aio_backend *backend, uint64_t base, uint64_t sectors); // 初始化没有借用者的新引擎。
int airi_aio_open(struct airi_aio_engine *engine, unsigned owner); // 新owner编号不能已有上下文。
int airi_aio_reset(struct airi_aio_engine *engine, unsigned owner); // 检查新epoch后撤销旧轮次，失败不改变旧状态。
void airi_aio_close(struct airi_aio_engine *engine, unsigned owner); // 撤销排队和CQ，ACTIVE保留到真实收集。
long airi_aio_buffer_alloc(struct airi_aio_engine *engine, unsigned owner); // 返回正代数句柄，失败负数。
long airi_aio_buffer_put(struct airi_aio_engine *engine, unsigned owner, uint64_t handle, const void *data, size_t bytes); // idle才可修改，尾部清零。
long airi_aio_buffer_get(struct airi_aio_engine *engine, unsigned owner, uint64_t handle, void *data, size_t bytes); // idle才可读，错误不会复制。
long airi_aio_buffer_free(struct airi_aio_engine *engine, unsigned owner, uint64_t handle); // busy拒绝释放。
int airi_aio_submit(struct airi_aio_engine *engine, unsigned owner, unsigned operation, uint64_t handle, uint64_t sector, uint64_t cookie); // 只排队与预留CQ信用，0不表示完成。
int airi_aio_pop(struct airi_aio_engine *engine, unsigned owner, struct airi_io_completion *completion); // 空返回-11，收集后才解锁缓冲。
int airi_aio_pump(struct airi_aio_engine *engine); // 有界一次服务，不睡眠；内部不变量错误-5。
bool airi_aio_work(const struct airi_aio_engine *engine); // 包含无owner的ACTIVE，不含仅等待用户收集的CQ。
void airi_async_demo(void); // 固定QEMU内核队列演示，用户API接下一检查点。
#endif // 结束保护。
```

**新建 `kernel/io/engine.c`。** 先把接受、设备借用、CQ发布与收集四个时刻分别实现。

<!-- write: kernel/io/engine.c -->
```c
#include <airi/async.h> // 固定池、请求副本与CQ信用由引擎自己拥有。
static struct airi_aio_owner *find_owner(struct airi_aio_engine *e, unsigned id) { // 编号0不是合法owner。
    if (!id) // 不把空槽误当有效身份。
        return NULL; // 所有公开接口再给明确错误。
    for (unsigned i = 0; i < AIRI_AIO_OWNERS; ++i) // 有界上下文集合。
        if (e->owners[i].id == id) // 同一编号当前只有一个epoch。
            return &e->owners[i]; // 内部借用，不交给用户。
    return NULL; // 已关闭或从未打开。
}
static struct airi_aio_buffer *find_buffer(struct airi_aio_engine *e, const struct airi_aio_owner *owner, uint64_t handle) { // 句柄先解索引，再核对完整代数和所有者。
    uint64_t index = (handle & 255u) - 1; // 低字节1–32，0会下溢并在下句拒绝。
    if (!owner || index >= AIRI_AIO_BUFFERS) // 不按原始值直接构造任意地址。
        return NULL; // 无效句柄。
    struct airi_aio_buffer *buffer = &e->buffers[index]; // 索引已限制。
    return buffer->state != AIRI_AIO_BUFFER_FREE && buffer->owner == owner->id && buffer->epoch == owner->epoch && buffer->handle == handle ? buffer : NULL; // 旧句柄和其他owner都不能命中。
}
int airi_aio_init(struct airi_aio_engine *e, const struct airi_aio_backend *backend, uint64_t base, uint64_t sectors) { // 仅用于无在途借用的新对象。
    if (!e || !backend || !backend->begin || !backend->poll || !sectors || base > UINT64_MAX - sectors) // 检查后端与有界扇区范围。
        return -22; // -EINVAL，旧对象不改。
    struct airi_aio_backend copy = *backend; // backend可以指向e内部，先保留。
    airi_memset(e, 0, sizeof(*e)); // 清静态池、代数与CQ。
    e->backend = copy; // 函数与context寿命由调用者保证。
    e->active = -1; // 没有硬件请求。
    e->base = base; e->sectors = sectors; // 用户可访问区域，不借正式日志的扇区。
    return 0; // 只有成功后才能使用其余接口。
}
int airi_aio_open(struct airi_aio_engine *e, unsigned id) { // 打开一个新的身份轮次。
    if (e->faulted) // 内部状态失配不得继续。
        return -5; // -EIO。
    if (!id || find_owner(e, id)) // 禁止0和重复owner。
        return -22; // -EINVAL。
    if (e->epoch == UINT64_MAX) // 不能回绕让旧请求命中新轮次。
        return -75; // -EOVERFLOW。
    for (unsigned i = 0; i < AIRI_AIO_OWNERS; ++i) // 寻找未使用上下文。
        if (!e->owners[i].id) { // 没有借用者的槽。
            e->owners[i] = (struct airi_aio_owner){.id = id, .epoch = ++e->epoch}; // 新epoch独立于槽号和ID。
            return 0; // CQ与信用初始为空。
        }
    return -11; // -EAGAIN，上下文容量满。
}
void airi_aio_close(struct airi_aio_engine *e, unsigned id) { // 活动请求不借owner对象地址，因此可以撤销上下文。
    struct airi_aio_owner *owner = find_owner(e, id); // 重复close安全无操作。
    if (!owner) // 无资源可撤销。
        return; // 不影响其他owner。
    for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 只取消尚未交给设备的排队项。
        if (e->requests[i].state == AIRI_AIO_QUEUED && e->requests[i].owner == id && e->requests[i].epoch == owner->epoch) // ACTIVE仍需被真实poll收集。
            airi_memset(&e->requests[i], 0, sizeof(e->requests[i])); // 未提交请求没有后端借用，可立即复用。
    for (unsigned i = 0; i < AIRI_AIO_BUFFERS; ++i) // 注册缓冲不被DMA直接使用。
        if (e->buffers[i].owner == id && e->buffers[i].epoch == owner->epoch) // 只撤销本轮次所有权。
            airi_memset(&e->buffers[i], 0, sizeof(e->buffers[i])); // 在途数据副本仍在ACTIVE单元中。
    airi_memset(owner, 0, sizeof(*owner)); // 丢弃CQ与信用，晚完成按epoch识别为无主并丢弃。
}
int airi_aio_reset(struct airi_aio_engine *e, unsigned id) { // 同一PID的新映像使用新epoch。
    if (e->faulted || !find_owner(e, id)) // 没有可替换的当前轮次。
        return -5; // 不改变旧状态。
    if (e->epoch == UINT64_MAX) // 在撤销旧轮次之前确认可以产生新身份。
        return -75; // -EOVERFLOW，旧请求和CQ仍有效。
    airi_aio_close(e, id); // 已有ACTIVE保留到poll，排队和旧句柄撤销。
    return airi_aio_open(e, id); // 自己刚释放了一个上下文槽，正常必可取得新epoch。
}
long airi_aio_buffer_alloc(struct airi_aio_engine *e, unsigned id) { // 固定注册池，数据不放入用户进程控制块的局部副本。
    struct airi_aio_owner *owner = find_owner(e, id); // 资源必须属于活轮次。
    if (!owner || e->faulted) // 没有可用引擎身份。
        return -5; // -EIO。
    unsigned count = 0; // 当前owner已有注册数。
    for (unsigned i = 0; i < AIRI_AIO_BUFFERS; ++i) // 每人最多四个，不能挤占全池。
        count += e->buffers[i].state != AIRI_AIO_BUFFER_FREE && e->buffers[i].owner == id && e->buffers[i].epoch == owner->epoch; // 同时核对epoch。
    if (count == 4) // 达到当前owner预算。
        return -28; // -ENOSPC。
    if (e->generation >= (1ull << 55) - 1) // 保持句柄最高符号位0且绝不回绕。
        return -75; // -EOVERFLOW。
    for (unsigned i = 0; i < AIRI_AIO_BUFFERS; ++i) // 搜索全局空槽。
        if (e->buffers[i].state == AIRI_AIO_BUFFER_FREE) { // 没有任何请求借注册对象。
            struct airi_aio_buffer *buffer = &e->buffers[i]; // 明确目标槽。
            airi_memset(buffer, 0, sizeof(*buffer)); // 新buffer没有上一个owner的数据。
            buffer->owner = id; buffer->epoch = owner->epoch; // 所有权绑定到本轮次。
            buffer->handle = (++e->generation << 8) | (i + 1u); // 新代数加非零索引，旧句柄不命中。
            buffer->state = AIRI_AIO_BUFFER_IDLE; // 用户可初始化，尚无请求借用。
            return (long)buffer->handle; // 保证为正的自有句柄。
        }
    return -28; // 全局池也可能耗尽。
}
long airi_aio_buffer_put(struct airi_aio_engine *e, unsigned id, uint64_t handle, const void *data, size_t bytes) { // 本函数接收的始终是内核或主机有效副本。
    if (e->faulted) // 内部状态失配后不继续修改或交出注册对象。
        return -5; // -EIO。
    struct airi_aio_buffer *buffer = find_buffer(e, find_owner(e, id), handle); // 完整句柄与轮次验证。
    if (!buffer) // 包括别的owner和已释放旧句柄。
        return -9; // -EBADF类句柄错误。
    if (buffer->state != AIRI_AIO_BUFFER_IDLE) // 在途或未收集CQ都不能覆盖。
        return -16; // -EBUSY。
    if (bytes > 512 || (!data && bytes)) // 零长度允许清空整buffer，非零必须有效。
        return -22; // -EINVAL。
    airi_memset(buffer->data, 0, 512); // 短输入尾部确定为零，不泄漏上次读数据。
    if (bytes) // NULL零长度不解引用。
        airi_memcpy(buffer->data, data, bytes); // 返回后不借data。
    return 0; // 不把容量误当完成长度。
}
long airi_aio_buffer_get(struct airi_aio_engine *e, unsigned id, uint64_t handle, void *data, size_t bytes) { // 复制已经可访问的固定buffer。
    if (e->faulted) // 内部状态失配后不继续修改或交出注册对象。
        return -5; // -EIO。
    struct airi_aio_buffer *buffer = find_buffer(e, find_owner(e, id), handle); // 只访问自己的当前句柄。
    if (!buffer) // 旧或外来身份拒绝。
        return -9; // 不复制输出。
    if (buffer->state != AIRI_AIO_BUFFER_IDLE) // CQ尚未收集前仍属请求生命周期。
        return -16; // -EBUSY。
    if (bytes > 512 || (!data && bytes)) // 输出容量先限制。
        return -22; // 不部分复制。
    if (bytes) // 允许只检查句柄的零长度请求。
        airi_memcpy(data, buffer->data, bytes); // 固定已初始化字节。
    return 0; // 上层用户复制另有完整范围检查。
}
long airi_aio_buffer_free(struct airi_aio_engine *e, unsigned id, uint64_t handle) { // 结束注册所有权。
    if (e->faulted) // 内部状态失配后不继续修改或交出注册对象。
        return -5; // -EIO。
    struct airi_aio_buffer *buffer = find_buffer(e, find_owner(e, id), handle); // 不能用其他owner句柄释放。
    if (!buffer) // 已释放或代数过期。
        return -9; // 不重复回收。
    if (buffer->state != AIRI_AIO_BUFFER_IDLE) // 已完成但CQ未收集也必须拒绝。
        return -16; // 保留完整生命周期。
    airi_memset(buffer, 0, sizeof(*buffer)); // 槽可复用，下一handle使用新全局代数。
    return 0; // 活动请求没有借此地址，IDLE证明可回收。
}
int airi_aio_submit(struct airi_aio_engine *e, unsigned id, unsigned operation, uint64_t handle, uint64_t sector, uint64_t cookie) { // 接收只入SQ，不直接等待设备。
    struct airi_aio_owner *owner = find_owner(e, id); // 当前轮次是所有后续关联依据。
    if (!owner || e->faulted) // 没有有效上下文。
        return -5; // -EIO。
    if ((operation != AIRI_IO_READ && operation != AIRI_IO_WRITE && operation != AIRI_IO_FLUSH) || (operation == AIRI_IO_FLUSH && handle)) // FLUSH没有数据缓冲。
        return -22; // 提交格式错误立即拒绝，不产生CQ。
    if (owner->outstanding == AIRI_AIO_CQ) // 每条接受请求必须先预留一个完成信用。
        return -11; // CQ未收集形成背压，即使SQ已空也不能无限提交。
    struct airi_aio_buffer *buffer = operation == AIRI_IO_FLUSH ? NULL : find_buffer(e, owner, handle); // 普通数据操作必须有注册对象。
    if (operation != AIRI_IO_FLUSH && !buffer) // 句柄错误不排队。
        return -9; // -EBADF。
    if (buffer && buffer->state != AIRI_AIO_BUFFER_IDLE) // 同一buffer不能同时属于两个请求。
        return -16; // -EBUSY。
    for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 当前轮次的在途cookie必须唯一。
        if (e->requests[i].state != AIRI_AIO_REQUEST_FREE && e->requests[i].owner == id && e->requests[i].epoch == owner->epoch && e->requests[i].cookie == cookie) // 排队和ACTIVE都算未完成身份。
            return -22; // 不让调用者得到无法区分的两个CQ。
    for (unsigned i = 0; i < owner->count; ++i) // 已完成未收集也仍占cookie。
        if (owner->completions[(owner->head + i) % AIRI_AIO_CQ].cookie == cookie) // 只读当前有效CQ项。
            return -22; // 收集后才可复用cookie。
    struct airi_aio_request *request = NULL; // 尚未修改任何buffer。
    for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 全局四条包含ACTIVE。
        if (e->requests[i].state == AIRI_AIO_REQUEST_FREE) { // 选择一个空槽。
            request = &e->requests[i]; // 还没有对外发布。
            break; // 只取得一个。
        }
    if (!request) // 全局SQ满与本owner信用不足分别存在。
        return -11; // 未接收，不改变buffer所有权。
    if (e->sequence == UINT64_MAX) // FIFO身份不能回绕混淆旧顺序。
        return -75; // -EOVERFLOW。
    *request = (struct airi_aio_request){.state = AIRI_AIO_QUEUED, .owner = id, .operation = operation, .epoch = owner->epoch, .handle = handle, .cookie = cookie, .sequence = ++e->sequence, .sector = sector}; // 请求头与零数据形成自有副本。
    if (buffer) { // 数据请求才锁buffer。
        if (operation == AIRI_IO_WRITE) // 设备将来读的是请求单元副本，不借注册地址。
            airi_memcpy(request->data, buffer->data, 512); // owner退出后这个副本仍由引擎持有。
        buffer->state = AIRI_AIO_BUFFER_BUSY; // 用户不可修改或释放，直到CQ收集。
    }
    ++owner->outstanding; // 接受时预留CQ容量，完成时不释放信用。
    return 0; // 请求可稍后开始，也可通过CQ报I/O错误。
}
static int complete(struct airi_aio_engine *e, struct airi_aio_request *request, int result) { // 后端已结束借用，才能回收请求单元。
    struct airi_aio_owner *owner = find_owner(e, request->owner); // 不保存已销毁进程指针。
    if (owner && owner->epoch == request->epoch) { // 同一ID的新轮次不能收到旧完成。
        struct airi_aio_buffer *buffer = request->operation == AIRI_IO_FLUSH ? NULL : find_buffer(e, owner, request->handle); // 代数与轮次再次核对。
        if (owner->count >= AIRI_AIO_CQ || !owner->outstanding || (request->operation != AIRI_IO_FLUSH && (!buffer || buffer->state != AIRI_AIO_BUFFER_BUSY))) // 这些都由提交信用和生命周期保证。
            return -5; // 内部不变量错误，调用者必须停止，不能覆盖未读CQ。
        if (!result && request->operation == AIRI_IO_READ) // 只有成功读取才更新注册buffer。
            airi_memcpy(buffer->data, request->data, 512); // 错误读保留原内容。
        unsigned tail = (owner->head + owner->count) % AIRI_AIO_CQ; // CQ环当前尾槽。
        owner->completions[tail] = (struct airi_io_completion){request->cookie, request->handle, result ? result : request->operation == AIRI_IO_FLUSH ? 0 : 512, request->operation, 0}; // cookie关联身份，不推断完成顺序。
        ++owner->count; // 结果完全写好才发布可收集数量。
    }
    airi_memset(request, 0, sizeof(*request)); // 无主完成丢弃，ACTIVE副本也只在真实收集后才复用。
    return 0; // 完成信用仍留到pop或owner关闭。
}
int airi_aio_pop(struct airi_aio_engine *e, unsigned id, struct airi_io_completion *completion) { // 调用者先准备有效内核输出。
    struct airi_aio_owner *owner = find_owner(e, id); // 仅取当前owner的CQ。
    if (!owner || e->faulted) // 旧轮次不能收集。
        return -5; // -EIO。
    if (!completion) // 输出非法不消费CQ。
        return -22; // -EINVAL。
    if (!owner->count) // 尚未形成完成结果。
        return -11; // 上层可选择阻塞而非用户忙等。
    struct airi_io_completion value = owner->completions[owner->head]; // 先保存当前有效项。
    struct airi_aio_buffer *buffer = value.operation == AIRI_IO_FLUSH ? NULL : find_buffer(e, owner, value.buffer); // flush没有注册对象。
    if (!owner->outstanding || (value.operation != AIRI_IO_FLUSH && (!buffer || buffer->state != AIRI_AIO_BUFFER_BUSY))) { // 不让损坏CQ解锁别人的buffer。
        e->faulted = true; // 后续提交不得继续使用失配状态。
        return -5; // 不消费或覆盖CQ。
    }
    if (buffer) // 错误完成同样结束请求对buffer的借用。
        buffer->state = AIRI_AIO_BUFFER_IDLE; // 此后才可get/put/free。
    airi_memset(&owner->completions[owner->head], 0, sizeof(value)); // 不保留重复可读的旧完成。
    owner->head = (owner->head + 1) % AIRI_AIO_CQ; // 消费环头。
    --owner->count; --owner->outstanding; // 收集同时释放CQ槽和预留信用。
    *completion = value; // 所有不变量成立后才交出结果。
    return 0; // 不是本条I/O的结果，具体字节/错误在completion.result。
}
int airi_aio_pump(struct airi_aio_engine *e) { // 只由普通内核服务调用，不在IRQ中睡眠或分配。
    if (e->faulted) // 上次内部不变量失配。
        return -5; // 不继续覆盖状态。
    if (e->active >= 0) { // 最多一个硬件请求在途。
        struct airi_aio_request *request = &e->requests[e->active]; // ACTIVE槽到poll结束前不得复用。
        int result = e->backend.poll(e->backend.context, request->data); // read落入请求自有副本。
        if (result == 1) // 设备仍在途，稍后服务再试。
            return 0; // pump本身成功不代表I/O已完成。
        if (result > 0) // 后端不应返回其他正状态。
            result = -5; // 作为明确错误完成，不能解释成字节数。
        if (complete(e, request, result)) { // 容量不变量不能静默破坏。
            e->faulted = true; // 保留失败身份，不能继续新提交。
            return -5; // 调用者应报告内部错误。
        }
        e->active = -1; // 真实收集后硬件位置才空闲。
    }
    for (unsigned attempt = 0; attempt < AIRI_AIO_REQUESTS; ++attempt) { // 最多处理四个立即失败，不无界循环。
        struct airi_aio_request *next = NULL; // 找最早接收的排队请求。
        for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 槽复用顺序不等于提交顺序。
            if (e->requests[i].state == AIRI_AIO_QUEUED && (!next || e->requests[i].sequence < next->sequence)) // 只在排队项之间比较稳定序号。
                next = &e->requests[i]; // 保留当前最早项。
        if (!next) // SQ已空，CQ可能仍未收集。
            return 0; // 不偷释放用户信用。
        int result; // 范围错误通过CQ交给已经接收的请求。
        if (next->operation != AIRI_IO_FLUSH && (next->sector < e->base || next->sector - e->base >= e->sectors)) // 禁止访问正式日志或实验区之外。
            result = -22; // 没有向设备提交，仍形成明确错误完成。
        else // 实际设备错误也走同一CQ路径。
            result = e->backend.begin(e->backend.context, next->operation, next->sector, next->data); // 后端只借ACTIVE请求副本。
        if (result == -16) // 可信后端当前由别的同步操作持有。
            return 0; // 请求仍在SQ，未丢身份与信用。
        if (!result) { // 只有被后端接收才变ACTIVE。
            next->state = AIRI_AIO_ACTIVE; // 数据副本保持到真实poll。
            e->active = (int)(next - e->requests); // 数组内指针差得到本槽。
            return 0; // 不在同次pump里等待它完成。
        }
        if (result > 0) // begin只允许0或负错误。
            result = -5; // 不能把未知正值当成功。
        if (complete(e, next, result)) { // 立即错误同样占用已预留CQ。
            e->faulted = true; // 内部失配不能继续。
            return -5; // 不覆盖未收集结果。
        }
    }
    return 0; // 本次服务预算用完，之后可再次pump。
}
bool airi_aio_work(const struct airi_aio_engine *e) { // 用于决定是否需要内核服务与退出排空。
    for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 包含没有owner但仍有硬件借用的ACTIVE。
        if (e->requests[i].state != AIRI_AIO_REQUEST_FREE) // 已完成CQ不在请求池内。
            return true; // 还不能丢弃引擎对象。
    return false; // 只有用户尚未收集的CQ不要求设备推进。
}
```

<a id="async-model"></a>
## 模型延后两次检查，再给实际结果

主机后端明确规定begin只保存请求，前两次poll返回1，之后才改模型介质或复制读结果。它不是伪造QEMU速度，而是为所有权测试提供可控制的在途时段。测试按cookie核对四条完成，验证全局SQ满、CQ信用满、重复cookie、外来handle、BUSY修改/释放和失败读保留原内容。

晚完成分别在reset同编号和close原owner后发生。新对象占用复用槽并写入new或foreign，旧请求真实经过模型poll之后，新CQ仍为空、新内容不变。32个buffer和8个上下文的耗尽与回收也明确检查。ASan/UBSan检查内存和未定义行为，但这些模型结论不替代下一段的真实设备证据。

**新建 `tests/unit/async_test.c`。** 断言对应资源交接和错误结果，不把运行通过记成读者掌握。

<!-- write: tests/unit/async_test.c -->
```c
#include <airi/async.h> // 与guest相同的有界提交与完成算法。
#include <assert.h> // 所有权、背压和晚完成都要具体核对。
#include <stdio.h> // 只输出最终完整结果。
struct fake_device { uint8_t disk[16][512], write[512]; bool pending, busy_once; unsigned operation, polls, begins; uint64_t sector; int next_error, active_error; }; // 明确延后完成的后端模型。
static struct fake_device device; // 模型介质和在途副本不在被释放的owner里。
static struct airi_aio_engine engine; // 固定池不复制成两个管理器。
static int begin(void *context, unsigned operation, uint64_t sector, const void *data) { // 与真实K30接口同样只接收。
    struct fake_device *d = context; // 后端自己拥有状态。
    if (d->busy_once) { // 一次外部忙拒绝，不能丢SQ项。
        d->busy_once = false; // 下次允许重试。
        return -16; // 尚未接收，没有数据借用。
    }
    assert(!d->pending); // 引擎最多一个ACTIVE。
    assert(operation == AIRI_IO_FLUSH || (sector >= 100 && sector < 116)); // 非法扇区必须在引擎内形成CQ，不能碰设备。
    d->pending = true; d->operation = operation; d->sector = sector; // 保存后端请求身份。
    d->active_error = d->next_error; d->next_error = 0; // 一次明确设备错误作用于这条请求。
    d->polls = 0; ++d->begins; // 每条需要两次待完成观察才完成。
    if (operation == AIRI_IO_WRITE) // 模型和真实驱动都先持有写副本。
        airi_memcpy(d->write, data, 512); // 不借注册buffer地址。
    return 0; // 接收不等于完成。
}
static int poll(void *context, void *data) { // 每次只检查一次。
    struct fake_device *d = context; // 与begin同一设备对象。
    assert(d->pending); // 不可偷取别人的或残留的完成。
    if (d->polls++ < 2) // 明确制造尚未完成，而不是伪造真实QEMU延迟。
        return 1; // 调用者应去做其他工作。
    d->pending = false; // 后端结束借用。
    if (d->active_error) // 设备错误不能当成功字节数。
        return d->active_error; // 引擎应将它放到CQ。
    if (d->operation == AIRI_IO_WRITE) // 写副本在完成时进入模型介质。
        airi_memcpy(d->disk[d->sector - 100], d->write, 512); // 块范围已经核对。
    if (d->operation == AIRI_IO_READ) // 读结果写到请求单元副本。
        airi_memcpy(data, d->disk[d->sector - 100], 512); // owner可以已经关闭，不借它的地址。
    return 0; // FLUSH在这个模型中只确认之前已经完成的写。
}
static void drain(void) { // 模型有固定两次pending，服务预算必须足够且有上界。
    unsigned steps = 0; // 避免错误状态在主机测试里无限循环。
    while (airi_aio_work(&engine)) { // CQ未收集不算硬件工作。
        assert(++steps < 100); // 若ACTIVE被提前释放或永远不推进，测试明确失败。
        assert(!airi_aio_pump(&engine)); // 内部容量不变量不能被覆盖。
    }
    assert(!device.pending); // 没有owner也要结束后端借用。
}
int main(void) { // host只验证明确模型，真设备由独立guest演示。
    struct airi_aio_backend backend = {&device, begin, poll}; // 绑定延后完成模型。
    assert(!airi_aio_init(&engine, &backend, 100, 16)); // 固定可访问模型扇区。
    assert(!airi_aio_open(&engine, 1) && !airi_aio_open(&engine, 2)); // 两个独立owner用于全局SQ满观察。
    long writer = airi_aio_buffer_alloc(&engine, 1), reader = airi_aio_buffer_alloc(&engine, 1), error_buffer = airi_aio_buffer_alloc(&engine, 1); // 同一owner三个对象。
    long extra = airi_aio_buffer_alloc(&engine, 2); // 另一个owner不受前者CQ信用限制。
    assert(writer > 0 && reader > 0 && error_buffer > 0 && extra > 0); // 句柄始终为正。
    uint8_t pattern[512], output[512]; // 全字节内容可辨认实际读写。
    for (unsigned i = 0; i < 512; ++i) // 不以全零掩盖未运行的写。
        pattern[i] = (uint8_t)(i * 31u + 7u); // 可复现模式。
    assert(!airi_aio_buffer_put(&engine, 1, writer, pattern, 512)); // idle注册缓冲可初始化。
    assert(airi_aio_buffer_get(&engine, 2, writer, output, 1) == -9); // 外来句柄不能跨owner访问。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_WRITE, writer, 100, 30)); // cookie不按数字顺序提交。
    assert(airi_aio_buffer_free(&engine, 1, writer) == -16 && airi_aio_buffer_put(&engine, 1, writer, pattern, 1) == -16); // 已排队即锁定生命周期。
    assert(airi_aio_submit(&engine, 1, AIRI_IO_FLUSH, 0, 0, 30) == -22); // 未完成cookie不可重复。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_FLUSH, 0, 0, 10)); // 同一硬件消费者保证写先完成再开始FLUSH。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_READ, reader, 100, 20)); // 后续读回实际模式。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_READ, error_buffer, 99, 40)); // 格式正确但范围错误，接收后由CQ报告。
    assert(airi_aio_submit(&engine, 2, AIRI_IO_READ, extra, 100, 50) == -11); // 另一个owner仍有信用，但全局四槽满。
    assert(!airi_aio_buffer_free(&engine, 2, extra)); // 未接收不能偷走buffer所有权。
    device.busy_once = true; // 第一次后端忙不应丢最早SQ项。
    assert(!airi_aio_pump(&engine) && !device.pending && airi_aio_work(&engine)); // 所有请求仍可推进。
    drain(); // 设备完成不等于用户已收集。
    assert(device.begins == 3); // 越界请求没有访问设备，其他三条真实进入后端。
    assert(airi_aio_buffer_free(&engine, 1, writer) == -16 && airi_aio_buffer_get(&engine, 1, reader, output, 1) == -16); // CQ未收集前仍BUSY。
    assert(airi_aio_submit(&engine, 1, AIRI_IO_FLUSH, 0, 0, 50) == -11); // SQ已空，四个CQ信用仍占用。
    struct airi_io_completion completion; // 结果记录含cookie、句柄、类型与具体错误。
    unsigned seen = 0; // 不依赖收集顺序，按cookie核对身份。
    for (unsigned i = 0; i < 4; ++i) { // 每条接受请求必须形成一条完成。
        assert(!airi_aio_pop(&engine, 1, &completion) && !completion.reserved); // 固定ABI保留字段为零。
        if (completion.cookie == 30) { assert(completion.buffer == (uint64_t)writer && completion.result == 512); seen |= 1; } // 写结果属于writer。
        else if (completion.cookie == 10) { assert(!completion.buffer && !completion.result); seen |= 2; } // FLUSH没有buffer。
        else if (completion.cookie == 20) { assert(completion.buffer == (uint64_t)reader && completion.result == 512); seen |= 4; } // 读结果属于reader。
        else { assert(completion.cookie == 40 && completion.buffer == (uint64_t)error_buffer && completion.result == -22); seen |= 8; } // 越界以错误完成交回。
    }
    assert(seen == 15 && airi_aio_pop(&engine, 1, &completion) == -11); // 无重复、无漏项、空CQ明确返回。
    assert(!airi_aio_buffer_get(&engine, 1, reader, output, 512) && !airi_memcmp(pattern, output, 512)); // 收集后才可读回内容。
    device.next_error = -5; // 另一条请求被设备接收，但最终报错。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_READ, reader, 100, 20)); // cookie收集后允许复用。
    drain(); // 真实模型延后路径后才有错误结果。
    assert(!airi_aio_pop(&engine, 1, &completion) && completion.result == -5); // 不是把提交返回0误当I/O成功。
    assert(!airi_aio_buffer_get(&engine, 1, reader, output, 512) && !airi_memcmp(pattern, output, 512)); // 失败读不覆盖上次已知内容。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_READ, reader, 100, 60)); // 让旧轮次真正ACTIVE后再关闭。
    assert(!airi_aio_pump(&engine) && device.pending); // 后端已经借请求单元，不能提前复用它。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_FLUSH, 0, 0, 61)); // 此排队项在reset时可以取消。
    assert(!airi_aio_reset(&engine, 1)); // 同一编号新epoch，旧ACTIVE仍留到收集。
    long replacement = airi_aio_buffer_alloc(&engine, 1); // 可复用旧buffer槽，但handle必须不同。
    assert(replacement > 0 && replacement != writer && airi_aio_buffer_get(&engine, 1, writer, output, 1) == -9 && airi_aio_buffer_get(&engine, 1, reader, output, 1) == -9); // 旧句柄不能命中新对象。
    assert(!airi_aio_buffer_put(&engine, 1, replacement, "new", 3)); // 新轮次有独立内容。
    drain(); // 旧ACTIVE读会完成，但不能写新buffer或新CQ。
    assert(airi_aio_pop(&engine, 1, &completion) == -11 && !airi_aio_buffer_get(&engine, 1, replacement, output, 3) && !airi_memcmp(output, "new", 3)); // 晚完成按epoch丢弃。
    assert(!airi_aio_submit(&engine, 1, AIRI_IO_READ, replacement, 100, 70)); // 再覆盖owner直接退出的情形。
    assert(!airi_aio_pump(&engine) && device.pending); // ACTIVE副本已经被后端持有。
    airi_aio_close(&engine, 1); // 退出撤销注册对象，ACTIVE请求单元不能提前归还。
    long foreign = airi_aio_buffer_alloc(&engine, 2); // 另一个owner可拿到同一个池槽的新句柄。
    assert(foreign > 0 && foreign != replacement && !airi_aio_buffer_put(&engine, 2, foreign, "foreign", 7)); // 新对象内容明确。
    drain(); // 旧owner缺失，晚完成应丢弃而非写新对象。
    assert(airi_aio_pop(&engine, 2, &completion) == -11 && !airi_aio_buffer_get(&engine, 2, foreign, output, 7) && !airi_memcmp(output, "foreign", 7)); // 不借已关闭owner地址。
    airi_aio_close(&engine, 1); airi_aio_close(&engine, 2); // 关闭无在途的全部owner。
    assert(!airi_aio_work(&engine)); // 不遗留排队资源。
    for (unsigned id = 1; id <= 8; ++id) { // 满上下文和注册池。
        assert(!airi_aio_open(&engine, id)); // 每次产生不同epoch。
        for (unsigned i = 0; i < 4; ++i) // 每owner最多四个。
            assert(airi_aio_buffer_alloc(&engine, id) > 0); // 全池32个均可用。
        assert(airi_aio_buffer_alloc(&engine, id) == -28); // 第五个失败不侵占别人的预算。
    }
    assert(airi_aio_open(&engine, 9) == -11); // owner池满与buffer池满分别报告。
    for (unsigned id = 1; id <= 8; ++id) // 释放全部代数对象。
        airi_aio_close(&engine, id); // 重复close安全不制造新信用。
    puts("PASS async broker: SQ/CQ backpressure, cookie identity, busy buffers, errors, owner reset and late completion"); // 模型结论与guest实测分开。
    return 0; // 全部断言成立。
}
```

<a id="async-real-device"></a>
## 四条真实请求共用一个硬件位置

service使用静态engine，避免把几十KiB池压进16KiB进程内核栈。演示owner7001/7002只是算法编号，尚未创建对应用户进程。独立块区256–271与正式日志16–143分开；先保存256号扇区，再提交cookie30写、cookie10 FLUSH、cookie20读回和cookie40越界到16。最后一条必须产生CQ−22而不接触正式日志。

第五条由另一owner提交仍得−11，证明全局四槽已满，不只是第一个owner信用不足。完成全积累到CQ后，SQ已空，再提交仍因未收集信用得−11；buffer释放仍为−16。收集四条后核对全部cookie、handle与result，读回pattern，再撤销owner并恢复原扇区、FLUSH。

当前硬件最多一个在途，所以这四条按提交顺序串行执行；身份关联仍明确保存，不能把本演示说成已经实现乱序设备队列或并行性能提升。只读/隔离设备明确跳过这条写读演示，用户错误完成接下一节。

**新建 `kernel/io/service.c`。** 真实后端适配与演示等待策略在这里，pump本身不睡眠。

<!-- write: kernel/io/service.c -->
```c
#include <airi/async.h> // 有限SQ/CQ与注册缓冲所有权。
#include <airi/block.h> // K30可信单消费者的begin/poll。
#include <airi/kernel.h> // 真设备演示与内部错误报告。
static struct airi_aio_engine engine; // 固定长期内核对象，不压进用户内核栈局部副本。
static int begin(void *context, unsigned operation, uint64_t sector, const void *data) { // 队列后端不认识用户PID或地址。
    (void)context; // 当前固定单块设备。
    return airi_block_begin(operation, sector, data); // 只接收，不等待完成。
}
static int poll(void *context, void *data) { // 完成落入请求单元自有副本。
    (void)context; // 与begin同一设备。
    return airi_block_poll(data); // 1待完成，0或负数结束。
}
void airi_async_demo(void) { // 先在内核独立证明队列，再接用户系统调用。
    if (!airi_block_sectors() || airi_block_readonly()) { // 故障镜像不伪装成功写读演示。
        airi_log("[ASYNC_QUEUE_SKIPPED] writable_block=0"); // 受限设备仍可继续管道与内存。
        return; // 用户错误完成路径在下一节单独接入。
    }
    struct airi_aio_backend backend = {NULL, begin, poll}; // 仅设备接口，不含用户对象指针。
    if (airi_aio_init(&engine, &backend, 256, 16) || airi_aio_open(&engine, 7001) || airi_aio_open(&engine, 7002)) // 演示owner编号不是已创建的用户进程。
        airi_panic("cannot initialize bounded async demo"); // 不发布半个队列。
    uint8_t before[512], pattern[512], output[512]; // 同步备份独立实验扇区，演示后恢复。
    if (airi_block_read(256, before)) // 队列尚未pump，没有硬件在途。
        airi_panic("cannot preserve async scratch sector"); // 不用未知备份覆盖设备。
    for (unsigned i = 0; i < 512; ++i) // 可辨认实际写入。
        pattern[i] = (uint8_t)(i * 29u + 5u); // 非零重复模式。
    long writer = airi_aio_buffer_alloc(&engine, 7001), reader = airi_aio_buffer_alloc(&engine, 7001), error_buffer = airi_aio_buffer_alloc(&engine, 7001); // 三个独立代数句柄。
    if (writer < 0 || reader < 0 || error_buffer < 0 || airi_aio_buffer_put(&engine, 7001, (uint64_t)writer, pattern, 512)) // 先初始化可修改的IDLE缓冲。
        airi_panic("cannot prepare async buffers"); // 不提交未初始化内容。
    if (airi_aio_submit(&engine, 7001, AIRI_IO_WRITE, (uint64_t)writer, 256, 30) || airi_aio_submit(&engine, 7001, AIRI_IO_FLUSH, 0, 0, 10) || airi_aio_submit(&engine, 7001, AIRI_IO_READ, (uint64_t)reader, 256, 20) || airi_aio_submit(&engine, 7001, AIRI_IO_READ, (uint64_t)error_buffer, 16, 40)) // 四条接收，非法范围通过CQ错误完成，不碰正式日志。
        airi_panic("async SQ did not accept bounded workload"); // 接收和I/O完成分开。
    if (airi_aio_submit(&engine, 7002, AIRI_IO_FLUSH, 0, 0, 50) != -11 || airi_aio_buffer_free(&engine, 7001, (uint64_t)writer) != -16) // 全局SQ满与已提交缓冲忙。
        airi_panic("async queue backpressure or buffer ownership failed"); // 不覆盖旧请求。
    while (airi_aio_work(&engine)) { // 设备仍有工作，CQ不被提前消费。
        if (airi_aio_pump(&engine)) // 每次有界检查，不在算法内睡眠。
            airi_panic("async engine invariant failed"); // 内部错误不能当普通设备结果。
        if (airi_aio_work(&engine)) // 下一次服务需要等待设备或PIT进展。
            __asm__ volatile("sti; hlt" ::: "memory"); // 这里只是启动演示的等待策略，不属于submit/pump接口。
    }
    if (airi_aio_submit(&engine, 7001, AIRI_IO_FLUSH, 0, 0, 50) != -11 || airi_aio_buffer_free(&engine, 7001, (uint64_t)writer) != -16) // SQ已空但四个CQ信用仍占用，buffer仍BUSY。
        airi_panic("uncollected CQ released resources too early"); // 硬件完成不等于调用者已收集。
    unsigned seen = 0; // 按cookie核对，不按返回位置猜请求。
    for (unsigned i = 0; i < 4; ++i) { // 每个接受请求必须有一个结果。
        struct airi_io_completion completion; // 固定32字节数值记录。
        if (airi_aio_pop(&engine, 7001, &completion)) // 收集才解锁对应buffer并归还信用。
            airi_panic("async CQ result missing"); // 不允许静默丢请求。
        if (completion.cookie == 30 && completion.buffer == (uint64_t)writer && completion.result == 512) // 写成功属于writer。
            seen |= 1; // 记录身份匹配。
        else if (completion.cookie == 10 && !completion.buffer && !completion.result) // FLUSH没有buffer。
            seen |= 2; // 真设备同步完成。
        else if (completion.cookie == 20 && completion.buffer == (uint64_t)reader && completion.result == 512) // 读结果属于reader。
            seen |= 4; // 稍后再核对字节。
        else if (completion.cookie == 40 && completion.buffer == (uint64_t)error_buffer && completion.result == -22) // 范围拒绝通过错误完成。
            seen |= 8; // 正式日志没有被当成用户块区访问。
        else // 不认识或不符的结果不能冒充成功。
            airi_panic("async CQ identity/result mismatch"); // 具体cookie与句柄必须关联。
    }
    if (seen != 15 || airi_aio_buffer_get(&engine, 7001, (uint64_t)reader, output, 512) || airi_memcmp(output, pattern, 512)) // 全部身份以及真实读回内容。
        airi_panic("async read data did not match committed write"); // 不只依赖status和计数。
    airi_aio_close(&engine, 7001); airi_aio_close(&engine, 7002); // 全部完成已收集，可释放注册对象与上下文。
    if (airi_aio_work(&engine) || airi_block_write(256, before) || airi_block_flush()) // 完成后才用同步接口恢复扇区。
        airi_panic("async demo did not restore scratch sector"); // 不隐瞒设备或清理错误。
    airi_log("[ASYNC_QUEUE_DEMO_OK] sq=4 cq=4 inflight=1 cookie=matched buffer_until_pop=busy error=-22 restored=1"); // 真实设备结果与主机晚完成模型分别记录。
}
```

**更新构建。** guest加入engine/service，host-test增加第八组明确后端模型。

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
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
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
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
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
# 设备超时故障镜像故意不发布avail.idx，正常镜像不含注入。
ifeq ($(CASE),block-timeout)
CPPFLAGS += -DAIRI_TEST_BLOCK_TIMEOUT # 验证真实PIT截止与DMA隔离，不冒充设备断电。
endif # 结束块设备故障配置。
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
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf $(OUT)/user/waits.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -DAIRI_STORAGE_ELF='"$(OUT)/user/storage.elf"' -DAIRI_PIPES_ELF='"$(OUT)/user/pipes.elf"' -DAIRI_WAITS_ELF='"$(OUT)/user/waits.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 多进程工作负载有自己的独立静态ELF。
$(OUT)/user/processes.elf: $(USER_COMMON) $(OUT)/user/processes.o labs/airios/program.ld
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# worker也有自己的_start和main，原始ELF只在包装时进入内核。
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 只链接这份工作负载的main。
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# storage与recover由同一ELF按明确argv分支。
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 管道程序只复用自有用户入口与库。
$(OUT)/user/pipes.elf: $(USER_COMMON) $(OUT)/user/pipes.o labs/airios/program.ld
# 保持相同低地址ELF布局。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# ELF只选择waits的main。
$(OUT)/user/waits.elf: $(USER_COMMON) $(OUT)/user/waits.o labs/airios/program.ld
# 低地址静态链接，不借宿主入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@
```

**更新启动顺序。** 正式日志scan后运行独立队列演示，再进入既有用户程序。

<!-- write: kernel/main.c -->
```c
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
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
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    int block_error = airi_block_init(info); // 固定单队列设备，未连接时明确停止当前存储阶段。
    if (block_error) { // 默认运行必须带独立实验盘。
        airi_log("[BLOCK_INIT_ERROR] error=%d", block_error); // 先记录具体设备错误。
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_journal_demo(); // 固定实验扇区上提交、校验、坏尾截断与恢复。
    int storage_error = airi_storage_init(); // 启动扫描，绝不自动追加正式记录。
    if (storage_error) { // I/O错误不能当作空盘。
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_demo(); // 三个真实设备操作和一个有界范围错误完成。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
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
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
        airi_log("[AIRI_P4_READY] files=ram pipes=blocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
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

**更新验证。** 可写设备要求真实队列标记，受限设备要求明确SKIPPED。

<!-- write: scripts/boot.py -->
```python
"""QEMU launch and bounded boot checks; success requires a kernel marker."""  # 固定虚拟机参数并按本轮内核标记判断启动结果
import json  # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import shutil      # 取得目录复制、文件复制与临时结果清理接口。
import socket      # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from tools import ROOT, tool  # ROOT 确定项目位置，tool 按已建立优先级寻找工具。

def command(profile, work, gui=False, debug=False, debug_port=1234):  # 返回 QEMU 参数列表；profile 选镜像，work 保存本轮可写固件与日志。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    firmware=ROOT/'third_party/firmware'    # 定位固定固件目录。
    variables=work/'ovmf-vars.fd'  # 本轮固件变量单独存放，避免改动锁定的原始模板。
    shutil.copyfile(firmware/'edk2-i386-vars.fd',variables)  # 复制变量模板，QEMU 可写副本而保持外部依赖原件。
    cmd=[tool('qemu-system-x86_64','qemu'),'-machine','q35','-accel','tcg',  # 先选 QEMU 路径与 q35/TCG，参数列表按元素边界传递。
         '-cpu','qemu64','-smp','1','-m','256M','-vga','std',  # 固定CPU型号、单核、256MiB及标准VGA。
         '-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={firmware/"edk2-x86_64-code.fd"}',  # 只读代码固件是第0个 pflash 单元，f 字符串填入实际路径。
         '-drive',f'if=pflash,format=raw,unit=1,file={variables}',  # 第1个 pflash 单元使用本轮可写变量副本。
         '-monitor','none','-cdrom',str(ROOT/'build'/profile/'airios.iso'),'-boot','d','-no-reboot',  # 挂载对应配置镜像，从光盘启动，故障时不循环重启。
         '-serial',f'file:{work/"serial.log"}']  # 默认 UART 输出保存到本轮 serial.log。
    disk = work / 'disk.raw' # 每个运行目录只使用项目自己的独立实验盘。
    if disk.is_symlink(): # 拒绝把实验写入重定向到外部文件。
        raise RuntimeError('Experiment disk cannot be a symlink') # 不跟随未知目标。
    if not disk.exists(): # 保留已有实验盘用于后续重启恢复观察。
        with disk.open('xb') as stream: # 排他创建，不覆盖竞争产生的文件。
            stream.truncate(8 * 1024 * 1024) # 稀疏零文件，固定8MiB。
    if not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024: # 当前设备容量固定，未知文件不交给guest写。
        raise RuntimeError('Expected an 8 MiB project experiment disk') # 要求显式处理错误，不自动truncate旧文件。
    drive = f'if=none,id=airi-disk,format=raw,file={disk},cache=writeback' # QEMU后端可缓存，持久化需客体FLUSH。
    if profile.removeprefix('desktop-') == 'block-readonly': # 故障演示使用真实只读设备特征。
        drive += ',readonly=on' # 不修改宿主文件的权限设置。
    if profile.removeprefix('desktop-') != 'block-missing': # 缺设备配置故意不接实验盘。
        cmd += ['-drive', drive, '-device', 'virtio-blk-pci,drive=airi-disk,disable-modern=on,vectors=0'] # 固定legacy与无MSI-X布局，现代接口另行学习。
    if not gui: cmd+=['-display','none']  # 无人操作时隐藏显示窗口，guest 仍可绘图并经 QMP 截帧。
    if debug: cmd+=['-S','-gdb',f'tcp:127.0.0.1:{debug_port}']  # -S 暂停CPU，GDB只监听回环地址指定端口。
    return cmd  # 仅返回参数列表，调用者决定何时启动与回收进程。
def qmp_execute(sockpath, execute, arguments=None):  # 本机连接只执行本次指定的控制请求。
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:  # 外层拥有底层连接，最后关闭。
        sock.settimeout(5)  # 连接、发送和读取最多阻塞五秒。
        sock.connect(str(sockpath))  # 路径定位QEMU控制端点，不是普通输出文件。
        with sock.makefile('rb') as stream:  # 内层包装按行读取字节，先关闭包装再关闭连接。
            json.loads(stream.readline())  # 消费服务器问候，JSON库直接接受bytes。
            result = None  # 尚未收到动作结果，不与空字典混淆。
            request = {'execute': execute}  # 当前控制动作保存为请求字段。
            if arguments is not None:  # 空字典也是显式参数，只有None表示不提供。
                request['arguments'] = arguments  # 将本次参数加到同一请求。
            for req in ({'execute': 'qmp_capabilities'}, request):  # 先协商能力再发实际动作。
                sock.sendall((json.dumps(req) + '\n').encode())  # 直到全部消息字节交出或失败，不假设一次write写完。
                while True:  # 跳过异步事件，等待当前请求的结果。
                    reply = json.loads(stream.readline())  # 按消息换行取得完整JSON字节。
                    if 'error' in reply:  # QEMU拒绝了当前请求。
                        raise RuntimeError(str(reply['error']))  # 保留服务器的具体错误。
                    if 'return' in reply:  # event不满足完成条件。
                        result = reply['return']  # 取得当前请求结果。
                        break  # 此请求已完成，进入下一个或返回。
            return result  # 返回实际动作结果，两个with仍按由内到外关闭资源。

def qmp_screenshot(sockpath, output):  # 把本轮截图目标路径交给QEMU控制端点。
    qmp_execute(sockpath, 'screendump', {'filename': str(output)})  # 控制显示设备保存PPM，不向UART发送命令。
def check(profile='normal',timeout=35):  # 每次启动最多35秒，成功条件由配置对应完成标记决定。
    work=ROOT/'artifacts'/('boot-'+profile)  # 不同配置分开保存启动证据。
    work.mkdir(parents=True,exist_ok=True)   # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 当前检查只读取这次启动的串口文件。
    serial.unlink(missing_ok=True)  # 删去旧日志；不存在不报错，旧 READY 不会使新启动提前通过。
    # AF_UNIX paths are capped at 104 bytes on macOS. Source exports can be deep.
    endpoints=tempfile.TemporaryDirectory(prefix='airios-qmp-',dir='/tmp')  # 在短/tmp路径建立控制端点，避免 macOS UNIX socket 路径过长。
    sockpath=Path(endpoints.name)/'qmp.sock'  # 临时目录中的 qmp.sock 命名本次控制连接。
    cmd=command(profile,work)+['-qmp',f'unix:{sockpath},server=on,wait=off']  # 在固定启动参数后追加控制端点，QEMU不等待客户端才启动。
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' # 正常配置等完整存储阶段，隔离盘只保留进程可用标记。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            while time.monotonic()-started<timeout:  # 按实际经过秒数限制整次启动等待。
                text=serial.read_text(errors='replace') if serial.exists() else ''  # 日志尚未创建时取空串；暂时不完整的字节以替代字符读取。
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal', 'stack-guard', 'block-missing'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        if case == 'block-missing': # 必须确认真实枚举结果为缺设备。
                            assert '[BLOCK_INIT_ERROR] error=-19' in text # 不能用其他初始化失败充数。
                        if case == 'stack-guard': # 坏栈必须真的转到双重故障的独立栈。
                            assert '[STACK_GUARD_PROBE]' in text and '[EXCEPTION] vector=8 error=0x0 ' in text # 不能把任意PANIC冒充保护页演示。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        if case != 'block-timeout': # 隔离盘不再允许新提交。
                            assert '[BLOCK_NONBLOCK_OK]' in text # begin接受、重叠拒绝和poll收集已运行。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        async_marker = '[ASYNC_QUEUE_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[ASYNC_QUEUE_DEMO_OK]' # 受限设备不冒充可写队列演示。
                        assert async_marker in text # 接收、忙与完成身份须符合当前设备。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        assert '[WAIT_USER_OK]' in text and '[WAIT_IDLE_OK]' in text and '[WAIT_DEMO_OK]' in text # 真阻塞、超时与现场继续均已完成。
                        assert text.index('[WAIT_TIMEOUT_OK]') < text.index('[WAIT_BUSY_DONE]') # 纯计算用户不能饿死等待超时。
                        assert '[PIPE_USER_OK]' in text # 有界流、背压、关闭与fork共享均已实际运行。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
                        assert f'graphics={int(graphics)}' in text  # 报告值须与本次选择的呈现实现一致。。
                        assert '[MEMORY_SUMMARY]' in text  # 完成之前应已记录实际内存总计。
                        if case=='edit-experiment':  # 修改实验额外核对替换后的文字。
                            assert 'I changed the output and rebooted' in text  # 确认运行的是实验配置的输出行为。
                        if graphics:  # 有图形的完成路径才请求显示面截图。
                            qmp_screenshot(sockpath,work/'screen.ppm')  # 在完整 READY 之后读取对应已完成画面。
                    result={'profile':profile,'passed':True,'expected_marker':expected,  # 记录配置、成功与本次实际等待的标记。
                            'seconds':round(time.monotonic()-started,2)}  # 记录经过秒数，round(...,2) 保留两位小数。
                    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
                    print(f'PASS boot/{profile} ({result["seconds"]}s)')  # 打印本次配置和耗时，而非以QEMU仍运行判定通过。
                    return result  # 当前配置标记与截图核对成功，返回这次启动记录。
                if process.poll() is not None:  # poll 非 None 表示主机 QEMU 已退出，不能继续等 guest 标记。
                    raise RuntimeError(f'QEMU exited before marker; see {work}')  # 提前退出时报告证据目录。
                time.sleep(0.1)  # 给虚拟CPU和日志写入100毫秒处理时间，再检查。
            raise RuntimeError(f'Boot timeout after {timeout}s; see {work}')  # 耗尽截止时间后明确失败，并保留该配置的日志。
        finally:  # 成功返回与异常离开都进入收尾。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是结束请求，随后仍需收回子进程。
                process.wait(timeout=5)  # 最多等待五秒，防止清理本身无限卡住。
            except subprocess.TimeoutExpired:  # QEMU没有在正常终止期限内退出。
                process.kill()  # 强制结束本轮仍存活的QEMU。
                process.wait()  # 收回已结束子进程的退出状态和资源。
            sockpath.unlink(missing_ok=True)  # 删除本轮控制端点，端点已消失时允许继续。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

```sh
./airios test # 八组主机观察、九种启动、三条输入及双启动恢复。
./airios run # 观察ASYNC_QUEUE_DEMO_OK，原run storage和run waits仍可用。
```

正常标记为sq=4、cq=4、inflight=1、cookie=matched、buffer_until_pop=busy、error=−22、restored=1。它证明真实设备接收、收集和内容往返；晚完成对新owner的影响由主机模型单独验证。当前用户还没有注册/提交接口，P5完整READY尚未发布，下一节将这些资源接到进程生命周期和同一次CQ等待之后。
