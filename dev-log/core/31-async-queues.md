# K31 · 提交队列可以满，完成还没收集也不能提前释放缓冲

K30 已将一次块操作拆成 begin 与 poll：前者交出请求，后者检查设备是否结束。但硬件当前只有一个在途位置，其他调用者不能把自己的数据覆盖进去。现在需要一层软件队列，先接住几条请求，依次交给设备，并把结果保留到提交者来收取。

先跟随一次写入：提交者注册一个 512 字节 buffer，放入模式数据，再提交写扇区 256、cookie30 的请求。接收成功只说明软件队列记住了它；稍后 pump 才让设备开始，设备结束以后还要形成完成记录，最后由 pop 交给提交者。下面分别保存这些阶段需要的状态，而不是把所有“返回 0”都当成已经写完。

本章先建立内核中的独立算法，再用真实 VirtIO 后端执行写、FLUSH、读回和越界错误。用户地址、系统调用与真实进程退出/exec 的接入留到下一节；本章用明确的主机模型检查资源撤销后的晚完成。

<a id="async-three-identities"></a>
## 一条完成需要找回请求，也需要找回它的资源

写入者附上的 cookie30 是本次请求身份，完成记录原样带回它。K25 的设备描述符链首 id0 则是驱动管理硬件队列的位置，同一个硬件位置可以先后服务许多软件请求，不能用 id0 代替 cookie，也不能按完成出现的位置猜请求。

注册 buffer 有另一个句柄 handle，用来找到哪份已登记的数据。它与 cookie 回答不同问题：一个是请求借哪份资源，一个是完成属于哪次操作。后面同一个 owner 尚未收取的请求中，cookie 必须唯一；收取以后才可复用。不同 owner 的资源不能互相访问。

共享 `io_abi` 只放固定宽度数值，不放进程内指针。submission 保存 sector 与 cookie；completion 带回 cookie、buffer、result、operation，reserved 置零。数据操作成功的 result 为 512，FLUSH 为 0，失败为负。submit 的 0 只是接收，pump 的 0 是本次服务没有内部失败，pop 的 0 是成功交出一条记录；实际 I/O 结果要继续读取记录中的 result。

**新建 `include/airi/io_abi.h`。** 下一节用户程序会使用这份自有记录，本章还不是 Linux ABI。

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
## 接收请求时，就要保证将来有地方放结果

软件引擎用四个请求单元保存 QUEUED 或 ACTIVE 请求，这里称为 SQ。每个单元保存操作数值和自己的 512 字节副本；写提交时复制注册 buffer 的内容，之后设备推进不再借用调用者传入的地址。注册池有 32 个 512 字节 buffer，最多八个 owner，每个 owner 最多注册四个。owner 是引擎的数字身份，本章还不是内部进程控制块指针。

设备已经结束，不表示提交者已经读过结果。若立即把所有位置都交给新请求，尚未读取的完成可能无处保存。因此每个 owner 另有四项 CQ 和四份完成信用：接受一条请求就增加 outstanding，先预留它将来需要的结果容量，直到 pop 收取才归还信用。

例如同一 owner 已接收四条请求，四次设备操作后来都结束，SQ 单元可以全空，CQ 却有四条未读结果，outstanding 仍是 4。此时第五条请求仍应返回 −11；如果只检查 SQ，就会允许结果无限积累。CQ 用 head/count 环保存有效结果，SQ 则按接收时的 sequence 找最早 QUEUED，物理槽复用顺序不等于提交顺序。这些都是内核内的固定结构，不是 Linux 共享内存环或零拷贝提交路径。

数据 buffer 从接受到 pop 始终 BUSY。即使设备已经完成，put/get/free 仍返回 −16；收取对应 CQ 后才恢复 IDLE，允许查看、覆盖或释放。没有接收的请求不改变原状态，原本 IDLE 的 buffer 继续 IDLE，不能把失败的 submit 当作需要等待的请求。

<a id="async-late-owner"></a>
## 提交者撤销资源，已经开始的设备操作仍需结束

本轮资源由 owner 与 epoch 一起识别。以后某个 PID exec 后编号可以相同，却已经是另一轮资源；旧请求不能写进新映像的 buffer 或 CQ。open 分配新 epoch，reset 在确认代数尚可增长后才撤销旧轮次，失败保留旧状态。

关闭 owner 时，尚未交给设备的 QUEUED 可以取消，注册 buffer 和 CQ 也可撤销；ACTIVE 却不能立刻清空。它保存的副本仍是后端 poll 的稳定输出位置，只有真实 poll 结束以后才能回收。完成时重新按 owner 编号查当前上下文，再比 epoch；原 owner 不存在或已换轮次，就丢弃旧完成，不交给新 CQ。

handle 还要区分同一注册槽前后创建的对象：低字节是池索引加 1，高位是单调 generation，复用槽会得到新值。完成重新核对 owner、epoch 和完整 handle，防止旧整数指向新 buffer。保存数值身份而不借内部控制块地址，也使关闭 owner 后的 ACTIVE 不会悬空解引用。

设备 DMA 的实际目标仍由 K30 驱动的静态请求持有；引擎单元与注册 buffer 之间另行复制。当前没有直接 DMA 到用户页，所以撤销提交者的资源与结束设备借用可以分开。以后若直接使用用户页，必须固定页面并等设备真正结束，不能照搬当前的撤销办法。

**新建 `include/airi/async.h`。** 内部池结构供算法、内核和主机模型使用，未来用户只接触 io_abi 数值。

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

## 先建立身份和资源的稳定查找

engine 的辅助查找先按 owner 编号取当前轮次，再按 handle 的低字节定位注册槽，核对完整 handle、owner 与 epoch。找不到就返回错误，不能把任意句柄当地址。初始化复制可信后端接口，记录可访问扇区范围；open、close、reset 管理一轮资源，关闭时仅撤销尚未交出的请求，ACTIVE 保留到后端结束。

**新建 `kernel/io/engine.c`，先写初始化、查找与 owner 生命周期：**

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
```

## 注册句柄必须区分同一槽的前后两份数据

注册先找全局空 buffer，再设置 owner 和 epoch，清除旧内容，生成新的 handle。若没有其他注册插入，槽 0 在 generation 为 1 时得到 `(1<<8)|1=257`，释放后复用且代数增为 2 时得到 513；低字节仍指向同一个槽，完整整数已经不同。generation 是全局增长值，实际下一次不一定恰好加一到这个示例值。

put 把传入字节复制到 IDLE 注册 buffer，并清零短输入的尾部；返回后不再借传入地址。get 同样只从 IDLE 复制，free 也要求 IDLE。分配检查每 owner 四个、全局 32 个的容量，代数不能回绕；资源不符或 BUSY 时不复制或释放。

**追加注册、复制与释放接口：**

<!-- append: kernel/io/engine.c -->
```c
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
```

## submit 接受后，哪些状态已经改变

回到 cookie30 的写请求。submit 先核对操作格式、owner 信用、句柄、IDLE 状态和 cookie 唯一性，再找全局空请求槽；任一条件失败都没有接受。检查全部成立后，写数据复制到请求单元，buffer 改 BUSY，outstanding 加一，单元成为带 sequence 的 QUEUED，然后返回 0。

提交给错 owner 的 handle 不会取得资源；重复 cookie 在 QUEUED、ACTIVE 和未收取 CQ 中都被拒绝。SQ 满或本 owner 信用不足返回 −11；非法操作格式立即 −22，不产生 CQ。sequence 也必须能继续增长，不能回绕改变 FIFO 判断。FLUSH 没有数据 buffer，要求 handle 为零，但同样占一份请求容量与完成信用。

**追加 `airi_aio_submit`：**

<!-- append: kernel/io/engine.c -->
```c
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
```

## 设备结束、发布 CQ 与收取是三个时刻

后端用 0 表示本次设备操作成功结束。complete 根据 operation 把它转换成记录里的结果：读写为 512，FLUSH 为 0；负值则保留为 I/O 错误。成功读先把请求副本复制进注册 buffer，错误读保留原内容。cookie、handle 和结果全部写入 CQ 后才增加 count，避免发布半条记录。

请求单元此时可回收，但 buffer 仍 BUSY，outstanding 也不减少。pop 先复制 CQ 头项并验证它与当前注册对象匹配，再解锁 buffer、移动 head、减少 count 和 outstanding，最后把完整值交给调用者。pop 返回 0 只表示取到了记录，调用者仍要检查 completion.result；没有结果返回 −11。内部信用、handle 或 BUSY 状态不一致属于引擎不变量破坏，必须停止，不能当作普通设备失败继续覆盖结果。

**追加 CQ 形成与 `airi_aio_pop`：**

<!-- append: kernel/io/engine.c -->
```c
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
```

## pump 一次推进多少，何时把控制权交回服务方

pump 先检查唯一 ACTIVE。poll 返回 1 表示仍在途，本次服务就返回 0，等待以后再检查；0 或负值才进入 complete。后端已经结束借用以后，ACTIVE 位置才释放。随后按 sequence 找最早 QUEUED，begin 接收成功便标 ACTIVE 并立即返回，本次不等待设备。

若后端暂忙 −16，请求保留在 SQ。若数据请求扇区不在允许范围，软件直接形成 CQ−22，不访问后端；因为 submit 已经接受过它，调用者必须从 CQ 收到结果，不能把它变成一次没有完成的消失请求。设备错误同样走 CQ。一次最多处理四个立即失败，避免坏输入令服务循环无界运行；内部不变量破坏才把引擎设 faulted 并要求内核停止。

`airi_aio_work` 看的是 QUEUED 或 ACTIVE，包括已经没有 owner 的 ACTIVE。只有未收取 CQ 时它可以为假，因为此时没有设备工作需要推进，但提交者仍有结果和信用未释放。服务方要据此区分“需要继续 pump”与“资源全部收齐”。

**追加 `airi_aio_pump` 与工作检查：**

<!-- append: kernel/io/engine.c -->
```c
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
## 用可控制的在途阶段检查撤销和复用

主机模型实现同一 begin/poll 接口。begin 只保留请求，前两次 poll 固定返回 1，之后才修改模型介质或交回读取字节。这使测试能在设备尚未结束时主动 close/reset，观察本章需要保护的阶段，不是在伪造 QEMU 的真实速度。

模型检查四条完成的 cookie，并分别触发全局 SQ 满、owner CQ 信用满、重复 cookie、外来 handle、BUSY 修改与释放以及错误读保留原内容。reset 同编号后创建新对象写入 new，或 close 后让别的 owner 复用槽写入 foreign；旧 ACTIVE 真实经过后续 poll 以后，新 CQ 仍应为空、新数据仍应不变。测试也借尽并回收 32 个 buffer 与八个上下文，防止只覆盖一次成功分配。

ASan/UBSan 另查内存和未定义行为，模型结果支持的是受控时序下的生命周期结论。下一段还需真实设备证明接收与字节往返，两种证据不互相替代。

**新建 `tests/unit/async_test.c`：**

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
## 把四条软件请求交给一个真实硬件位置

service 用长期静态 engine，避免把几十 KiB 固定池放进 16 KiB 任务栈。它只把引擎的 begin/poll 接到 K30 块接口；本次 7001、7002 是算法 owner 编号，还没有创建相应用户进程。

实验范围为扇区 256–271，正式日志在 16–143。先同步保存 256 号扇区，再创建 writer、reader 和 error_buffer 三个句柄，初始化 writer 的模式数据。四项属于 owner7001：

| cookie | 请求与资源 | 期望的 completion.result |
| --- | --- | --- |
| 30 | writer 写扇区 256 | 512 |
| 10 | FLUSH，无 buffer | 0 |
| 20 | reader 读扇区 256 | 512 |
| 40 | error_buffer 读扇区 16 | −22，软件拒绝越界，不碰正式日志 |

先全部 submit，不立即 pop。owner7002 再提交第五条 FLUSH 也得 −11，这次它自身尚无信用占用，说明拒绝来自全局四槽。演示循环 pump，必要时用时钟唤醒后的下一轮继续检查，等待策略在 service，不放进 submit 或 pump。

设备操作全结束后，四个 SQ 单元已回收，但 owner7001 四份完成仍在 CQ；再次提交 FLUSH 应因信用满得到 −11，writer 的 free 仍为 −16。之后逐条 pop，按 cookie 和 handle 核对各自结果，收完才能 get reader 并与 pattern 比较。最后关闭两 owner，恢复保存的 256 号扇区并 FLUSH，不能让一次观察改变实验之前的内容。

当前后端最多一个在途位置，四条按提交顺序串行执行。cookie 仍明确记录身份，不能由此宣称已实现硬件乱序、并行队列或性能提升。只读或隔离设备会明确跳过写读演示；用户接口下的错误完成接下一章。

**新建 `kernel/io/service.c`：**

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

## 接入构建和启动，分别保留两种观察

guest 增加 engine/service，host-test 增加第八组 async 模型。前者连接真实设备，后者控制晚完成时序，不能只运行其中一个就认为另一条路径已验证。

**更新 `Makefile`。** 原有规则保留，按下面唯一的旧文本窗口替换。

**guest 源文件列表：第一块定位旧文本，用第二块完整替换。**

<!-- replace: Makefile -->
<!-- before -->
```makefile
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```makefile
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

**主机 async 模型目标：第一块定位旧文本，用第二块完整替换。**

<!-- replace: Makefile -->
<!-- before -->
```makefile
# 模型不声称真实主机掉电，真设备另由guest演示。
	./build/tests/journal_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```
<!-- after -->
```makefile
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
```

正式日志 scan 完成后才执行独立队列演示，再进入原有用户程序。这样实验只使用单独扇区，不与恢复流程或旧程序同时借用硬件位置。

**更新 `kernel/main.c`，保留其余初始化与交互路径：**

**异步接口头文件：第一块定位旧文本，用第二块完整替换。**

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
```
<!-- after -->
```c
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
```

**日志恢复后的队列演示调用：第一块定位旧文本，用第二块完整替换。**

<!-- replace: kernel/main.c -->
<!-- before -->
```c
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
```
<!-- after -->
```c
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_demo(); // 三个真实设备操作和一个有界范围错误完成。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
```

启动观察对可写设备要求真实队列成功标记，对受限设备要求明确 SKIPPED；只有探测文字或预期的错误码都不足以证明该路径执行过。

**更新 `scripts/boot.py`，保留既有检查：**

**按设备条件选择队列观察标记：第一块定位旧文本，用第二块完整替换。**

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
```
<!-- after -->
```python
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        async_marker = '[ASYNC_QUEUE_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[ASYNC_QUEUE_DEMO_OK]' # 受限设备不冒充可写队列演示。
                        assert async_marker in text # 接收、忙与完成身份须符合当前设备。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
```

## 对照实际完成与资源归还

下面仍使用已有入口运行模型、启动和输入回归，再进入 normal 镜像观察队列标记：

```sh
./airios test # 八组主机观察、九种启动、三条输入及双启动恢复。
./airios run # 观察ASYNC_QUEUE_DEMO_OK，原run storage和run waits仍可用。
```

正常标记为 `sq=4 cq=4 inflight=1 cookie=matched buffer_until_pop=busy error=−22 restored=1`：软件同时接住四项，设备一次一项，完成按 cookie 与 buffer 对上，直到 pop 才解除 BUSY，越界请求交回错误，实验扇区最终恢复。它记录真实设备接收、完成和内容往返；owner 撤销后的晚完成由前面的主机模型另行验证。

当前用户还不能直接注册或提交，也尚未发布完整 P5 READY。下一节需要把同一轮资源绑定到进程，并让等待 CQ 的那一次系统调用在结果到来后继续；本章已经区分的接受、设备结束和收取，届时仍要分别保留。
