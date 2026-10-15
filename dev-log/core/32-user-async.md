# K32 · 用户请求离开调用栈之后，谁把完成交回来

从K31继续。本节让真正ring3程序申请注册buffer、提交四条请求、等待CQ并验证结果，同时让另一个用户继续执行。随后在请求已开始或完成时退出、exec，确认旧资源不会交给新轮次。最后以QEMU真实限速证明一次CQ阻塞期间另一个任务取得CPU，而不要求快设备也人为等待。

<a id="async-user-abi"></a>
## 用户有两类对象，寿命不同

用户提交结构只有sector和cookie，内核在int80返回前复制完，所以用户函数submit里的局部结构可立即结束寿命，下次调用可以复用同一地址。注册buffer却从接受请求开始保持BUSY，直到对应CQ被收集；用户put/get/free都会在BUSY时返回−16。完成前可复用提交描述，不可复用请求借用的数据资源，这两个规则不能混在一起。

airi_io_alloc返回正句柄，不是C指针。当前注册的是内核固定512字节池，put/get显式复制；设备DMA仍用驱动静态区。它不是Linux io_uring注册用户页的零拷贝接口，也不宣称兼容IORING_REGISTER_BUFFERS。范围仍限制在256–271独立实验扇区，正式日志不会被这个用户接口覆盖。

编号23申请，24 put，25 get，26 free；27提交时传操作、handle和16字节描述地址；28 next接受CQ输出与wait标志，0不等、1阻塞；29提供只读状态。next返回0表示复制出一条完成，真正I/O结果在completion.result。负CQ结果、空队列−11、坏输出−14与提交未接受是不同层次的结果。

新状态记录为40字节：queued、active、completed、registered_buffers、busy_buffers、outstanding、retired_active、reserved八个32位数，再接64位epoch。retired_active只表示同一PID旧epoch尚由内核收集的ACTIVE数量，不授权读取旧数据或CQ。所有结构都没有内核指针，可供以后串口、图形或WebUI适配消费，但还不是网络协议。

**更新公开数值记录。** 用户与内核都检查固定长度，当前程序仍以C11编译。

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
struct airi_io_state { // 只读有界快照，不含队列或内核指针。
    uint32_t queued, active, completed, registered_buffers, busy_buffers, outstanding; // 本owner当前轮次的实际数值。
    uint32_t retired_active, reserved; // 同一ID旧epoch仍由内核收集的ACTIVE，不交旧结果给新映像。
    uint64_t epoch; // 可观察同一PID的映像资源轮次。
};
_Static_assert(sizeof(struct airi_io_state) == 40, "state ABI size"); // 两端按同一固定长度复制。
_Static_assert(sizeof(struct airi_io_submission) == 16, "submission ABI size"); // 读用户结构前固定字节数。
_Static_assert(sizeof(struct airi_io_completion) == 32, "completion ABI size"); // CQ输出不能随编译器结构变化。
#endif // 结束保护。
```

<a id="async-observer"></a>
## 轨迹来自真实状态变化，观察器不拥有请求

引擎新增可选event回调。接受SQ后报告submit，后端接收后报告start，CQ形成后报告complete；旧epoch或无主完成报告drop，关闭时尚未开始的请求报告cancel。观察器只在当前调用内借请求元数据，不能保存指针或重入修改引擎。它不影响CQ信用与设备所有权，不把数据内容或内核地址写进串口。

内部state按owner与epoch统计当前请求与buffer，另外数同PID的旧ACTIVE；quiescent核对全部owner、注册buffer和请求单元。静态池不会改变物理页free数，因此仅比较页数不足以发现注册资源泄漏。新主机断言在reset后确认当前active=0、retired_active=1、outstanding=0，后端收集后retired_active归零。

**更新算法与内核服务声明。** 内部观察接口和公开io_abi分开，不向用户交池布局。

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
#define AIRI_AIO_SUBMIT 1u // SQ接受后，尚未进入设备。
#define AIRI_AIO_START 2u // 后端begin实际接收。
#define AIRI_AIO_COMPLETE 3u // 当前owner的CQ形成。
#define AIRI_AIO_DROP 4u // 无主或旧epoch完成被丢弃。
#define AIRI_AIO_CANCEL 5u // owner关闭时撤销尚未开始的项。
struct airi_aio_request; // 观察回调只在当前调用内借请求元数据。
struct airi_aio_backend { // 可信单消费者后端，begin不等待、poll只检查一次。
    void *context; // 后端自有状态寿命覆盖引擎。
    int (*begin)(void *, unsigned, uint64_t, const void *); // 0接收，-16忙，其他负错误形成CQ。
    int (*poll)(void *, void *); // 1在途、0完成、负错误。
    void (*event)(void *, unsigned, const struct airi_aio_request *, long); // 可选同步观察，只读且不得重入或修改引擎。
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
int airi_aio_state(struct airi_aio_engine *engine, unsigned owner, struct airi_io_state *state); // 当前owner轮次的只读数值。
bool airi_aio_quiescent(const struct airi_aio_engine *engine); // 所有owner、buffer和在途单元均已释放。
void airi_async_init(void); // 所有设备配置都初始化资源管理器，后端错误通过CQ报告。
int airi_async_open(unsigned pid); // 新进程空上下文，不继承父注册buffer。
int airi_async_reset(unsigned pid); // exec提交前换epoch。
void airi_async_close(unsigned pid); // 退出撤销owner，ACTIVE留内核收集。
long airi_async_buffer_alloc(unsigned pid); // 当前PID自己的代数句柄。
long airi_async_buffer_put(unsigned pid, uint64_t handle, const void *data, size_t bytes); // 内核副本输入。
long airi_async_buffer_get(unsigned pid, uint64_t handle, void *data, size_t bytes); // 内核副本输出。
long airi_async_buffer_free(unsigned pid, uint64_t handle); // BUSY拒绝。
int airi_async_submit(unsigned pid, unsigned operation, uint64_t handle, const struct airi_io_submission *submission); // 已复制的提交数值。
int airi_async_pop(unsigned pid, struct airi_io_completion *completion); // 空CQ为-11。
int airi_async_state(unsigned pid, struct airi_io_state *state); // 提供给用户或未来显示消费者的数值快照。
bool airi_async_work(void); // 包含无主ACTIVE。
void airi_async_pump(void); // 只在长期内核服务栈推进。
void airi_async_drain(void); // 最后用户退出后结束内核持有的ACTIVE，并核对完全回收。
void airi_async_demo(void); // 固定QEMU内核队列演示，用户API接下一检查点。
#endif // 结束保护。
```

**按完整版本更新引擎。** K31接受/收集顺序保留，只加入同步只读观察、状态与完全回收检查。

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
static void event(struct airi_aio_engine *e, unsigned stage, const struct airi_aio_request *request, long result) { // 同步只读观察，不把内部指针保留到回调之后。
    if (e->backend.event) // 主机模型可以不安装观察器。
        e->backend.event(e->backend.context, stage, request, result); // 观察者不得重入修改引擎。
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
        if (e->requests[i].state == AIRI_AIO_QUEUED && e->requests[i].owner == id && e->requests[i].epoch == owner->epoch) { // ACTIVE仍需被真实poll收集。
            event(e, AIRI_AIO_CANCEL, &e->requests[i], -125); // 记录撤销而非伪造设备完成。
            airi_memset(&e->requests[i], 0, sizeof(e->requests[i])); // 未提交请求没有后端借用，可立即复用。
        }
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
    event(e, AIRI_AIO_SUBMIT, request, 0); // 已接受不表示I/O完成。
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
        event(e, AIRI_AIO_COMPLETE, request, owner->completions[tail].result); // 真实CQ已形成，记录对应结果。
    } else // 原owner退出或已经进入新epoch。
        event(e, AIRI_AIO_DROP, request, result ? result : request->operation == AIRI_IO_FLUSH ? 0 : 512); // 后端已结束，完成不会交给新对象。
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
            event(e, AIRI_AIO_START, next, 0); // 所有在途状态完整发布后才记录开始。
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

int airi_aio_state(struct airi_aio_engine *e, unsigned id, struct airi_io_state *state) { // 复制有限数值，不暴露池地址。
    struct airi_aio_owner *owner = find_owner(e, id); // 只统计当前轮次。
    if (!owner || e->faulted) // 无有效身份。
        return -5; // -EIO。
    if (!state) // 无效内核输出不修改引擎。
        return -22; // -EINVAL。
    *state = (struct airi_io_state){.completed = owner->count, .outstanding = owner->outstanding, .epoch = owner->epoch}; // CQ计数与信用分别表示。
    for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 请求身份按owner与epoch筛选。
        if (e->requests[i].owner == id && e->requests[i].epoch == owner->epoch) { // 旧ACTIVE不冒充新轮次的在途。
            state->queued += e->requests[i].state == AIRI_AIO_QUEUED; // 接受但尚未开始。
            state->active += e->requests[i].state == AIRI_AIO_ACTIVE; // 已交后端且未收集。
        } else if (e->requests[i].owner == id && e->requests[i].state == AIRI_AIO_ACTIVE) // 旧epoch仍占后端，但不属于新映像请求。
            ++state->retired_active; // 只给计数，不能读取旧buffer或CQ。
    for (unsigned i = 0; i < AIRI_AIO_BUFFERS; ++i) // 只读当前注册集合。
        if (e->buffers[i].owner == id && e->buffers[i].epoch == owner->epoch && e->buffers[i].state != AIRI_AIO_BUFFER_FREE) { // 排除空槽和其他身份。
            ++state->registered_buffers; // 总注册数。
            state->busy_buffers += e->buffers[i].state == AIRI_AIO_BUFFER_BUSY; // 含完成但CQ尚未收集。
        }
    return 0; // 结果只在本次调用时刻成立。
}
bool airi_aio_quiescent(const struct airi_aio_engine *e) { // 关闭会话后验证静态池也已回收，不只看物理页统计。
    if (e->faulted || e->active >= 0 || airi_aio_work(e)) // 无主ACTIVE也必须收集完。
        return false; // 不可销毁或重建引擎。
    for (unsigned i = 0; i < AIRI_AIO_OWNERS; ++i) // CQ与信用属于owner。
        if (e->owners[i].id) // 活owner说明资源尚未释放。
            return false; // 不能把空SQ当全部结束。
    for (unsigned i = 0; i < AIRI_AIO_BUFFERS; ++i) // 注册池由内核长期存储，页数不变也可能泄漏。
        if (e->buffers[i].state != AIRI_AIO_BUFFER_FREE) // 任一占用都不是quiescent。
            return false; // 交给调用者报告。
    return true; // 单调代数保留不是资源占用。
}
```

**更新主机模型。** 模型可不安装观察器，仍实际验证退役状态和晚完成的新对象内容。

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
    struct airi_aio_backend backend = {&device, begin, poll, NULL}; // 绑定延后完成模型。
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
    struct airi_io_state state; // 只读快照须准确区分新轮次与退役请求。
    assert(!airi_aio_state(&engine, 1, &state) && !state.active && state.retired_active == 1 && !state.outstanding && !state.registered_buffers); // 不把旧ACTIVE算成新请求或继承buffer。
    long replacement = airi_aio_buffer_alloc(&engine, 1); // 可复用旧buffer槽，但handle必须不同。
    assert(replacement > 0 && replacement != writer && airi_aio_buffer_get(&engine, 1, writer, output, 1) == -9 && airi_aio_buffer_get(&engine, 1, reader, output, 1) == -9); // 旧句柄不能命中新对象。
    assert(!airi_aio_buffer_put(&engine, 1, replacement, "new", 3)); // 新轮次有独立内容。
    drain(); // 旧ACTIVE读会完成，但不能写新buffer或新CQ。
    assert(!airi_aio_state(&engine, 1, &state) && !state.retired_active); // 后端结束后退役计数也归零。
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
    assert(airi_aio_quiescent(&engine)); // 静态owner、buffer和ACTIVE全部归还。
    puts("PASS async broker: SQ/CQ backpressure, cookie identity, busy buffers, errors, owner reset and late completion"); // 模型结论与guest实测分开。
    return 0; // 全部断言成立。
}
```

<a id="async-service-stack"></a>
## IRQ只决定服务时机，普通启动栈推进设备

submit只入SQ。普通系统调用尾部poll_waiters可以取已经形成的CQ，但不直接pump设备。时钟调度发现IO_WAIT或任何异步工作时暂回启动栈；launch的服务循环先pump一次，再重查等待者，然后公平选择用户。没有用户可运行则真正等PIT。即使另一个用户纯计算不调用系统调用，设备仍能获得周期服务。

airi_arch_idle把sti/hlt留在x86适配，airi_arch_user_gate把int129留在同一架构层。user_gate现在是一次普通C调用，调用期间它的返回地址和陷入frame都在长期启动栈上；暂回后函数返回，旧栈空间可再使用，下一次进入用户前重新调用登记新继续点。进程层仍负责区分暂时服务与最终退出。CS选择子、用户frame与页表仍是当前x86实现，增加两个适配函数不表示已经能直接换开发板。

**更新架构接口。** 通用服务调用等待与调度门，不散布具体指令。

<!-- write: include/airi/arch.h -->
```c
#ifndef AIRI_ARCH_H  // 避免重复声明架构操作。
#define AIRI_ARCH_H  // 标记本头文件已展开。
void airi_arch_relax(void);  // 在忙等的一次迭代间提示处理器；不是任务睡眠。
void airi_arch_idle(void); // 固定平台开IRQ并等待一次真实中断，调用者醒后重查条件。
long airi_arch_user_gate(void); // 进入当前用户切换门，返回可为暂回服务或最终结束，由进程层区分。
#endif  // 结束重复包含保护。
```

**更新当前x86实现。** CPU指令与返回值的具体约定在这里。

<!-- write: kernel/arch/x86_64/cpu.c -->
```c
#include <airi/arch.h>  // 通用循环只通过此声明使用处理器提示。
void airi_arch_relax(void) {  // 本实现只用于当前 x86-64 目标。
    __asm__ volatile("pause");  // 告知 x86 处理器正在忙等，不开启中断或切换任务。
}

void airi_arch_idle(void) { // x86等待细节留在架构层，通用服务不直接散布sti/hlt。
    __asm__ volatile("sti; hlt" ::: "memory"); // 开IRQ后等待PIT，返回仍允许IRQ，CS0不被用户抢占。
}
long airi_arch_user_gate(void) { // 调用期间本函数的启动栈现场保持，暂回后由调用者判状态。
    long result; // RAX接门返回数值。
    __asm__ volatile("int $0x81" : "=a"(result) :: "memory", "cc"); // 自有DPL0调度门，非用户系统调用ABI。
    return result; // 下一次恢复用户前会重新调用，登记新的继续现场。
}
```

**更新真实设备适配。** 所有配置先初始化引擎；普通服务负责pump，最后退出负责drain并核对quiescent。

<!-- write: kernel/io/service.c -->
```c
#include <airi/arch.h> // 空闲等待由当前CPU适配提供。
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
static void observe(void *context, unsigned stage, const struct airi_aio_request *request, long result) { // 当前调用内只读元数据，不保存指针或重入引擎。
    (void)context; // 单实例无需另存观察器状态。
    const char *phase = stage == AIRI_AIO_SUBMIT ? "submit" : stage == AIRI_AIO_START ? "start" : stage == AIRI_AIO_COMPLETE ? "complete" : stage == AIRI_AIO_DROP ? "drop" : "cancel"; // 阶段区分接收、设备与撤销。
    airi_log("[IO_TRACE] owner=%u epoch=%llu cookie=%llu buffer=%llu operation=%u sector=%llu phase=%s result=%ld", request->owner, (unsigned long long)request->epoch, (unsigned long long)request->cookie, (unsigned long long)request->handle, request->operation, (unsigned long long)request->sector, phase, result); // 不输出用户数据或内核地址。
}
void airi_async_init(void) { // 即使设备只读或已隔离，资源管理器也有明确初始状态。
    struct airi_aio_backend backend = {NULL, begin, poll, observe}; // 真设备与只读同步观察器分别适配。
    if (airi_aio_init(&engine, &backend, 256, 16)) // 固定独立实验区，不访问正式日志。
        airi_panic("cannot initialize async engine"); // 内部配置不合法不能继续。
}
int airi_async_open(unsigned pid) { // fork和新会话取得空owner上下文。
    return airi_aio_open(&engine, pid); // 不继承父注册buffer。
}
int airi_async_reset(unsigned pid) { // exec先确认新epoch可用，再撤销旧映像资源。
    struct airi_io_state state; // 只保存数值，重置后旧对象不再可借。
    if (!airi_aio_state(&engine, pid, &state) && state.outstanding) // 有活动资源才记录迁移细节。
        airi_log("[IO_OWNER_RESET] pid=%u queued=%u active=%u completed=%u", pid, state.queued, state.active, state.completed); // 说明本次真正碰到哪些阶段。
    return airi_aio_reset(&engine, pid); // 失败在撤销前返回，旧映像仍可继续。
}
void airi_async_close(unsigned pid) { // 退出与回滚可重复调用，不提前回收ACTIVE单元。
    struct airi_io_state state; // 关闭前的真实轮次快照。
    if (!airi_aio_state(&engine, pid, &state) && (state.outstanding || state.registered_buffers)) // 只在有资源时报告。
        airi_log("[IO_OWNER_CLOSE] pid=%u queued=%u active=%u completed=%u", pid, state.queued, state.active, state.completed); // 不预设退出时设备必然仍在途。
    airi_aio_close(&engine, pid); // ACTIVE持有自有副本，晚完成按epoch丢弃。
}
long airi_async_buffer_alloc(unsigned pid) { // 注册固定内核buffer，用户只拿句柄。
    return airi_aio_buffer_alloc(&engine, pid); // 代数与owner验证由算法统一处理。
}
long airi_async_buffer_put(unsigned pid, uint64_t handle, const void *data, size_t bytes) { // 输入已在进程边界复制。
    return airi_aio_buffer_put(&engine, pid, handle, data, bytes); // BUSY不能覆盖。
}
long airi_async_buffer_get(unsigned pid, uint64_t handle, void *data, size_t bytes) { // 输出先到内核副本。
    return airi_aio_buffer_get(&engine, pid, handle, data, bytes); // CQ收集前不可读取。
}
long airi_async_buffer_free(unsigned pid, uint64_t handle) { // 只结束注册所有权，不涉及用户页分配器。
    return airi_aio_buffer_free(&engine, pid, handle); // 旧句柄与BUSY分别拒绝。
}
int airi_async_submit(unsigned pid, unsigned operation, uint64_t handle, const struct airi_io_submission *submission) { // 描述已复制，不借用户结构。
    return airi_aio_submit(&engine, pid, operation, handle, submission->sector, submission->cookie); // 只接受进SQ，结果由CQ交回。
}
int airi_async_pop(unsigned pid, struct airi_io_completion *completion) { // 复制一条当前轮次结果。
    return airi_aio_pop(&engine, pid, completion); // 空为-11，可由进程层登记等待。
}
int airi_async_state(unsigned pid, struct airi_io_state *state) { // 同步只读数值供串口或未来呈现消费者使用。
    return airi_aio_state(&engine, pid, state); // 不交出内核对象指针。
}
bool airi_async_work(void) { // IRQ调度只查询，绝不在IRQ内pump。
    return airi_aio_work(&engine); // 无主ACTIVE仍需普通服务推进。
}
void airi_async_pump(void) { // 只有长期内核服务栈调用本适配。
    if (airi_aio_pump(&engine)) // 设备错误在CQ，返回负数只表示内部不变量失败。
        airi_panic("async engine invariant failed"); // 不能静默覆盖资源。
}
void airi_async_drain(void) { // 所有用户关闭后，内核仍拥有可能尚在途的单元。
    while (airi_aio_work(&engine)) { // backend截止由首次begin固定，ACTIVE不会被重用。
        airi_async_pump(); // 收集或超时后按旧epoch丢弃。
        if (airi_aio_work(&engine)) // 还需要设备或PIT进展。
            airi_arch_idle(); // 等真实中断，不无限忙等。
    }
    if (!airi_aio_quiescent(&engine)) // 静态池泄漏不会出现在页数差里，需要另查。
        airi_panic("async session leaked owner or registered buffer"); // 全部owner与注册资源都应已关闭。
}
void airi_async_demo(void) { // 先在内核独立证明队列，再接用户系统调用。
    if (!airi_block_sectors() || airi_block_readonly()) { // 故障镜像不伪装成功写读演示。
        airi_log("[ASYNC_QUEUE_SKIPPED] writable_block=0"); // 受限设备仍可继续管道与内存。
        return; // 用户错误完成路径在下一节单独接入。
    }
    if (airi_aio_open(&engine, 7001) || airi_aio_open(&engine, 7002)) // 演示owner编号不是已创建的用户进程。
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
            airi_arch_idle(); // 这里只是启动演示的等待策略，不属于submit/pump接口。
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

<a id="async-process-owner"></a>
## fork复制整数，exec与exit处理资源所有权

新进程完成映像与PID准备后才open异步owner；fork给孩子一个空上下文，不继承父注册buffer。孩子用户栈里的父handle整数虽然复制了，内核按孩子PID检查仍返回−9。这里是自有接口的明确策略，与Linux共享ring/FD的规则分开。

exec先准备完整新ELF，再reset当前PID的owner。reset在撤销旧轮次前确认可以产生新epoch，失败则清next并返回原调用，旧映像与I/O资源仍有效；成功后旧QUEUED与CQ撤销，ACTIVE副本留在内核。old临时控制块的async_owned必须清false，next才持新轮次，避免cleanup旧页表时顺便关闭新owner。PID计数也在回绕前拒绝，不让0或旧身份重新出现。

exit立即close owner并清async_owned，关闭文件后进入僵尸；ACTIVE可能仍在设备上，既不借用户页也不借已销毁owner指针。所有用户结束后先回收其页面和栈，再由长期内核对象drain无主ACTIVE，最后检查静态池完全回收。已开始写请求的副作用可能在用户退出后继续发生，退出不是撤销已发出的设备写；本节生命周期演示使用读请求，不暗中改变正式日志。

CQ等待先验证完整32字节用户输出，再尝试pop；空且wait=1时保存该用户地址、原frame并设IO_ASYNC。之后在目标自己的页表上复制完成，写frame.RAX=0，再设RUNNABLE。只读或非法输出必须在消费CQ之前拒绝，不能丢结果、归还信用或提前解锁buffer。

**按完整版本更新进程路径。** 接口、owner标志、fork/exec/exit、CQ等待和服务循环要一起更新；相同旧代码保留。

<!-- write: kernel/process/user.c -->
```c
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
#define USER_STACK 0x70000000ull // 当前每进程一页初始用户栈。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    struct user_page owned[64]; // 所有用户数据页，含代码、静态数据、堆页和栈。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
    uint64_t fault_error, heap_next; // 用户故障证据与下一次有界用户页地址。
    int exit_code; // 自有wait接口返回普通退出码或负故障编码。
    enum user_state state; // 只有RUNNABLE可被调度器选中。
    int64_t wait_target; // 正PID表示指定孩子，-1表示任意孩子。
    uint64_t wait_status; // 已验证的父进程用户输出地址，阻塞期间父进程不会修改映射。
    enum io_operation io; // 只有USER_IO_WAIT消费下面的请求副本。
    unsigned io_fd, io_events; // FD属于本进程表，events只用于就绪等待。
    uint64_t io_address, io_bytes, io_deadline; // 用户页在阻塞期间仍由进程拥有，deadline按真实tick。
    struct airi_trap_frame initial; // 尚未运行时的寄存器值，不依赖临时C对象。
    struct airi_trap_frame *frame; // 运行后指向本进程内核栈的实际现场。
};
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
static unsigned io_block_count, io_wake_count, idle_halt_count; // 会话真实I/O阻塞与唤醒次数。
static bool session_idle, service_from_timer; // 是否暂回启动栈服务，和整个会话结束分开。
static bool session_active; // 只有用户会话活动时才接受用户IRQ或系统调用。
static int session_result; // 根用户退出结果，不用最后一个孩子的结果覆盖。
static struct airi_trap_frame *kernel_return; // 启动栈一直保留到整个用户会话结束。
extern const uint8_t airi_user_probe_start[], airi_user_probe_end[]; // 受控正常映像。
extern const uint8_t airi_user_bad_start[], airi_user_bad_end[]; // 用户cli故障。
extern const uint8_t airi_user_load_bad_start[], airi_user_load_bad_end[]; // 用户读supervisor页故障。
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
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
static bool create_stack(struct user_process *p) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + slot(p) * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&p->stack, address, 4); // 完整成功后才借给用户根。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
        return false; // 零长度也保留本项目地址策略。
    for (size_t done = 0; done < bytes;) { // 跨越的每一页都必须满足权限。
        unsigned flags; // 当前叶子权限。
        if (!airi_vm_lookup(&p->space, address + done, NULL, &flags) || !(flags & AIRI_VM_USER) || (write && !(flags & AIRI_VM_WRITE))) // 输出指针还要求用户可写。
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
        airi_vm_lookup(&p->space, address + done, &physical, NULL); // 完整检查后页表在本次内核操作中保持不变。
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
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 本阶段有界等待队列由进程槽直接表示。
        struct user_process *p = &processes[i]; // 控制块在等待期间不会回收。
        if (p->state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (p->io == IO_ASYNC) { // 原CQ等待仍保留同一个用户输出地址。
            struct airi_io_completion completion; // 先取得内核数值副本，不借用户指针给引擎。
            result = airi_async_pop(p->pid, &completion); // 空CQ为-11，不代表I/O结果。
            if (result == -11) // 尚未形成可收集结果。
                continue; // 设备由启动服务栈推进，不在这里忙等。
            if (!result && !copy_user(p, &completion, p->io_address, sizeof(completion), true)) // 等待前已验证，单线程阻塞期间映射不能改变。
                airi_panic("validated completion output mapping changed"); // 不能消费CQ后静默丢结果。
        } else if (p->io == IO_READY) { // 订阅的是条件，不是一次成功read的承诺。
            result = airi_fd_ready(&p->files, p->io_fd); // 当前条件可能被其他进程消费或改变。
            if (result >= 0) // 负错误必须原样唤醒，不能按位筛掉。
                result &= p->io_events | AIRI_READY_HUP | AIRI_READY_ERROR; // 对端关闭即使未显式请求也应可见。
            if (!result && airi_timer_ticks() < p->io_deadline) // 无条件成立且尚未超时。
                continue; // 保持等待，之后由状态变化或PIT服务重查。
        } else { // 阻塞read/write重新尝试同一个有界请求。
            result = file_transfer(p, p->io_fd, p->io_address, (size_t)p->io_bytes, p->io == IO_WRITE, false, 0); // 用户buffer仍由p持有。
            if (result == -11) // 暂时依旧没有进展。
                continue; // 不把原系统调用提前返回给用户忙等。
        }
        p->frame->rax = (uint64_t)result; // 用户返回位置仍在原调用之后。
        p->io = IO_NONE; // 这一次等待不再持有请求地址。
        p->io_address = p->io_bytes = p->io_deadline = 0; // 清过期的用户对象与截止值。
        p->state = USER_RUNNABLE; // 结果就绪之后才发布运行资格。
        ++io_wake_count; // 实际唤醒，不把查询次数计成完成。
        airi_log("[WAIT_WAKE] pid=%u result=%ld", p->pid, result); // 普通内核服务栈输出，不在IRQ里打印。
    }
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    if (p->async_owned) { // 失败构造或最终收拢都只关闭真正拥有的owner。
        airi_async_close(p->pid); // ACTIVE副本归内核，取消不释放其后端目标。
        p->async_owned = false; // 局部副本与重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 回滚或已退出空表均安全，不能遗留全局引用。
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (p->count) // 用户代码与用户栈数据页分别归还。
        if (!airi_memory_return_page(p->owned[--p->count].physical)) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 不能在finish中提前做这一步。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
}
static void initial_frame(struct user_process *p, uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
    if (name) { // C程序使用本项目最小argc/argv栈协议。
        size_t bytes = airi_strlen(name) + 1; // 包含用户字符串的终止零。
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&p->space, USER_STACK, &physical, NULL)) // 栈必须已建立。
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
    p->frame = &p->initial; // 先保存独立寄存器值，place_frame稍后复制进实际内核栈。
    airi_memset(p->frame, 0, sizeof(*p->frame)); // 首次没有旧GPR。
    p->frame->rip = entry; // ELF入口或已知汇编入口。
    p->frame->cs = 27; // 用户代码段与RPL3。
    p->frame->ss = 35; // 用户数据段与RPL3。
    p->frame->rsp = stack_pointer; // 使用刚准备的用户布局。
    p->frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_process *p, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (p->count == 64) // 包括初始栈在内的全部数据页预算。
        return false; // 不越过所有权数组。
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) // 耗尽时没有需要登记的页。
        return false; // 调用方回滚已有部分。
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    if (!airi_vm_map(&p->space, address, physical, flags | AIRI_VM_USER)) { // 单页映射失败还没有发布到所有权数组。
        airi_memory_return_page(physical); // 明确归还本次未接入数据页。
        return false; // 调用者回滚此前成功的部分。
    }
    p->owned[p->count++] = (struct user_page){address, physical, flags | AIRI_VM_USER}; // 保存VA/PA/权限供fork复制与精确回收。
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return -8; // -ENOEXEC，旧输出和调用者原上下文都未改变。
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!airi_memory_user_space(&p->space)) // 创建独立低半区。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
        unsigned flags = ((segment->flags & 2) ? AIRI_VM_WRITE : 0) | ((segment->flags & 1) ? AIRI_VM_EXEC : 0); // ELF PF标志转换成本内核权限枚举。
        uint64_t first = segment->address & ~4095ull; // 覆盖段首所在页。
        uint64_t end = (segment->address + segment->memory_bytes + 4095) & ~4095ull; // 包含完整BSS跨度。
        for (uint64_t address = first; address < end; address += 4096) // 所有页面先清零再接入。
            if (!owned_page(p, address, flags)) // 物理耗尽或映射失败都终止本次装载。
                goto failed; // 清理根/表后再归还数据页。
        for (uint64_t done = 0; done < segment->file_bytes;) { // 只复制filesz字节，memsz剩余继续保持零。
            uint64_t physical; // lookup结果包含当前页内偏移。
            if (!airi_vm_lookup(&p->space, segment->address + done, &physical, NULL)) // 已映射跨度必须可查询。
                goto failed; // 内部装载状态不一致也不运行。
            uint64_t part = 4096 - ((segment->address + done) & 4095); // 当前页内可复制长度。
            if (part > segment->file_bytes - done) // 不从文件右端外复制字节。
                part = segment->file_bytes - done; // 最后一次缩到实际剩余。
            airi_memcpy(airi_memory_direct(physical), view.image + segment->offset + done, (size_t)part); // 内核控制的物理别名可初始化最终只读/可执行页。
            done += part; // 下一次继续剩余文件内容。
        }
    }
    if (!owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 栈单独RW/NX，不采用ELF可执行栈请求。
        goto failed; // 失败仍统一回滚。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 长期根仍活动，用户空间从未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
    return -12; // -ENOMEM，不提供部分映像。
}
static void place_frame(struct user_process *p) { // 初始寄存器值完成后复制到自己拥有的内核栈。
    uint64_t top = p->stack.virtual_address + p->stack.count * 4096ull; // TSS以后从同一个栈顶压入真实现场。
    p->frame = (void *)(uintptr_t)(top - sizeof(*p->frame)); // 与公共入口的176字节布局一致。
    *p->frame = p->initial; // 不借用临时C结构或其他进程的内核栈。
}
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(p->owned[0].physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
    cleanup(p); // 完整回滚。
    return false; // 不运行半个上下文。
}
static struct user_process *find_pid(unsigned pid) { // PID与数组槽分开，退出回收后不会匹配旧身份。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历有界进程表。
        if (processes[i].state != USER_UNUSED && processes[i].pid == pid) // 只查仍存活或待回收的身份。
            return &processes[i]; // 内核内部借用，不向用户交指针。
    return NULL; // 未找到活身份。
}
static struct user_process *unused(void) { // 准备中的槽不会被发布为RUNNABLE。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 固定容量避免无界内核分配。
        if (processes[i].state == USER_UNUSED) // 调用者在不可抢占内核中独占准备。
            return &processes[i]; // 后续失败必须清掉全部所有权。
    return NULL; // 表满返回明确失败。
}
static void collect_pending(void) { // 只在新的用户陷入入口调用，不在旧进程退出的C调用中释放其栈。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 回收父进程已取得状态的孩子。
        if (&processes[i] != current && processes[i].state == USER_REAP_PENDING) // 当前真正运行的内核栈不能在此回收。
            cleanup(&processes[i]); // 非活动根和已离开的内核栈可安全解除。
}
enum schedule_reason { REASON_TIMER, REASON_YIELD, REASON_WAIT, REASON_EXIT, REASON_IO }; // 原因用于验证真实抢占，不混成主动yield。
static struct airi_trap_frame *select_user(struct user_process *p) { // 只在完整陷入现场边界切根和TSS。
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->stack.virtual_address + p->stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->frame); // 用户返回地址与标志必须仍符合边界。
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
    if (old->state == USER_RUNNING) // 等待或退出不能误改回可运行。
        old->state = USER_RUNNABLE; // 时间片结束仍可运行。
    if (reason != REASON_TIMER) // 普通内核边界可重试等待条件。
        poll_waiters(); // 已有字节、关闭或错误形成后再唤醒。
    else { // IRQ只检查状态，并把设备推进交给启动服务栈。
        bool service = airi_async_work(); // 即使无人等待，已接收请求也不能被纯计算用户饿死。
        for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 管道、有限条件与CQ等待都需要普通服务。
            service |= processes[i].state == USER_IO_WAIT; // 不在IRQ里复制、分配或打印。
        if (service) // 完整用户frame已经保存。
            return pause_for_service(true); // 由启动栈pump与重查，再进入下一个用户。
    }
    for (unsigned n = 1; n <= PROCESS_COUNT; ++n) { // 从old之后轮转一圈。
        struct user_process *next = &processes[(slot(old) + n) % PROCESS_COUNT]; // PID与槽仍分开。
        if (next->state == USER_RUNNABLE) { // 仅可运行者有可返回的用户现场。
            if (next != old && reason == REASON_TIMER) // 真IRQ切不同身份。
                ++timer_switches; // 主动阻塞不冒充抢占。
            return select_user(next); // 汇编恢复目标frame。
        }
    }
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 没有可运行者时检查仍在等待的活进程。
        if (processes[i].state == USER_WAITING || processes[i].state == USER_IO_WAIT) // 可能正在等设备、超时或未来对端。
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
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
        session_result = code; // 孩子退出不改变根结果。
    struct user_process *parent = find_pid(p->parent); // 父身份可能已退出或不存在。
    if (parent && parent->state == USER_WAITING && (parent->wait_target == -1 || (uint64_t)parent->wait_target == p->pid)) { // 只有真正等待此孩子的父进程可以被唤醒。
        bool copied = !parent->wait_status || copy_user(parent, &code, parent->wait_status, sizeof(code), true); // 通过父进程自己的页表写结果。
        parent->frame->rax = copied ? p->pid : (uint64_t)-14ll; // 返回子PID或明确地址错误。
        parent->state = USER_RUNNABLE; // 结果形成后才公开可运行状态。
        parent->wait_target = 0; // 这一次等待已经结束。
        parent->wait_status = 0; // 不保留用户局部对象的过期地址。
        if (copied) // 地址错误时保留僵尸，让父进程能用有效指针重试。
            p->state = USER_REAP_PENDING; // 下一次别的用户陷入后才能释放本次仍在用的内核栈。
    }
    airi_log("[USER_EXIT] pid=%u code=%d user_ticks=%u", p->pid, code, p->user_ticks); // 正常系统调用上下文记录身份结果。
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    if (next_pid == UINT32_MAX) // 固定32位正PID不允许回绕命中旧身份。
        return -75; // -EOVERFLOW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = &parent->owned[i]; // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(child->owned[child->count - 1].physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    child->heap_next = parent->heap_next; // 复制后的用户页分配游标也属于子空间。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
    place_frame(child); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->count); // 实际复制数量含用户栈与堆。
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
        if (child->state == USER_UNUSED || child->state == USER_REAP_PENDING || child->parent != current->pid || (target != -1 && (uint64_t)target != child->pid)) // 不允许等待别人的子进程。
            continue; // 查找下一个可能孩子。
        found = true; // 至少有一个真实孩子。
        if (child->state == USER_ZOMBIE) { // 已结束孩子可以在父内核栈上立即回收。
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
    current->wait_target = target; // 保存要等待的身份条件。
    current->wait_status = status; // 父阻塞期间单线程不会修改这些用户映射。
    current->state = USER_WAITING; // 先记录条件与结果地址，再让调度器跳过父。
    ++wait_blocks; // 真实阻塞，而非轮询返回成功。
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
static long allocate_user(size_t bytes) { // 按页提供有界用户内存，不是完整malloc。
    if (!bytes || bytes > 16384) // 一次最多四页，防止无界请求。
        return -22; // -EINVAL。
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > 64 - current->count) // 保留所有权数组容量。
        return -12; // -ENOMEM。
    uint64_t address = current->heap_next; // 首次从ELF范围之外开始。
    unsigned before = current->count; // 失败只回滚本次新增映射。
    for (unsigned i = 0; i < count; ++i) // 页物理可以不连续，用户VA连续。
        if (!owned_page(current, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 游标尚未前移。
    current->heap_next += count * 4096ull; // 全部成功后再发布新的末端。
    return (long)address; // 当前没有逐对象free，进程结束统一回收。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
        const struct user_page *page = &current->owned[--current->count]; // 先退到最后一项。
        if (!airi_vm_unmap(&current->space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    return -12; // 不返回半段用户范围。
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
    struct user_process next = {0}; // 临时所有者不在进程表中，尚不能被调度。
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    int async_error = airi_async_reset(current->pid); // 新ELF完整后，提交前确认可以进入新资源epoch。
    if (async_error) { // reset失败发生在撤销前，旧映像与旧I/O资源仍有效。
        cleanup(&next); // next尚未拥有当前PID的owner。
        return async_error; // 原用户调用可继续处理失败。
    }
    struct user_process old = *current; // 暂存旧用户资源，切离旧根之后归还。
    old.async_owned = false; // reset已撤销旧轮次，cleanup旧映像不能关闭新的owner。
    next.async_owned = true; // 同一PID的新epoch由提交后的控制块拥有。
    unsigned old_pages = old.count; // 归还前保留实际替换规模用于记录。
    next.pid = old.pid; // exec保持进程身份。
    next.parent = old.parent; // 父仍等待同一个孩子。
    next.user_ticks = old.user_ticks; // 同一身份的已运行计数不丢失。
    next.heap_next = USER_HEAP; // 新映像的动态用户页从空开始。
    next.stack = old.stack; // 明确转移正在使用的内核栈所有权，VA和物理内容都不变。
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
    *f = next.initial; // 事务提交时才覆盖旧用户RIP/RSP和寄存器。
    next.files = old.files; // 文件表所有权从旧映像转到新映像，偏移和普通FD继续存在。
    airi_memset(&old.files, 0, sizeof(old.files)); // cleanup旧映像不能重复关闭已转移引用。
    airi_fd_close_all(&next.files, true); // 只有成功提交才关闭CLOEXEC，失败exec不影响原表。
    next.state = USER_RUNNING; // 此身份继续运行，但从新_start开始。
    *current = next; // 把新所有权发布到原进程槽，PID不变。
    if (!airi_vm_activate(&current->space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    cleanup(&old); // 旧根已经不活动，旧数据可回收；当前内核栈已转移所以不释放。
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", current->pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 不解引用错误地址。
        return exit_current(-22); // 只结束拥有这个现场的用户。
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    current->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->user_ticks; // 实际CPU用户时间片事件数。
    if (current->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
        return schedule(REASON_TIMER); // 没有其他可运行者时恢复同一个进程。
    return airi_user_resume(f); // 也不能跳过坏返回检查。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // 内核仍不可抢占，只在完整用户陷入边界处理进程状态。
    if (f->vector == 129 && f->cs == 8 && (!session_active || session_idle) && current && current->state == USER_RUNNABLE) { // 内核启动一组已经准备好的用户。
        kernel_return = f; // 启动栈的继续位置一直保留。
        session_idle = false; // 当前int129重新登记了仍在启动栈上的继续位置。
        session_active = true; // 之后的CS3陷入有会话归属。
        return select_user(current); // 按目标根和TSS进入首个用户。
    }
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
        current->fault_error = f->error; // 保存权限故障证据。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // 同步文件与流的完整副本边界。
        bool write = f->rax == 1 || f->rax == 16; // 输出到流或文件。
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
        long result = file_transfer(current, f->rdi, f->rsi, (size_t)f->rdx, write, positioned, f->r10); // 先实际尝试一次。
        if (result == -11 && !positioned && !airi_fd_nonblocking(&current->files, (unsigned)f->rdi)) { // 阻塞流暂不可完成，用户不能提前看到EAGAIN。
            current->io = write ? IO_WRITE : IO_READ; // 保存需要重试的操作种类。
            current->io_fd = (unsigned)f->rdi; // 此时FD已由file_transfer完整验证。
            current->io_address = f->rsi; // 用户页继续归此进程所有，不借内核局部buffer。
            current->io_bytes = f->rdx; // 一次最多256字节，已经验证跨度。
            current->state = USER_IO_WAIT; // 条件和数据先登记，再移出可运行集合。
            ++io_block_count; // 实际阻塞计数。
            airi_log("[WAIT_REGISTER] pid=%u operation=%s fd=%u", current->pid, write ? "write" : "read", current->io_fd); // 普通系统调用上下文记录。
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
                current->io = IO_READY; // 不读写任何字节，只重查条件。
                current->io_fd = (unsigned)f->rdi; // 属于此进程的稳定表项。
                current->io_events = (unsigned)f->rsi; // 保存订阅条件。
                current->io_deadline = airi_timer_ticks() + f->rdx; // 真实时钟截止，不靠循环次数。
                current->state = USER_IO_WAIT; // 原frame已保存在进程中。
                ++io_block_count; // 这次调用确实阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=ready fd=%u", current->pid, current->io_fd); // 由普通内核上下文记录。
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
                current->io = IO_ASYNC; // poll_waiters只收集，设备推进在启动栈。
                current->io_address = f->rdi; // 用户输出页仍由进程拥有。
                current->io_bytes = sizeof(completion); // 固定ABI长度，不借C局部对象。
                current->state = USER_IO_WAIT; // 先保存条件和输出，再撤销运行资格。
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
        if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
        return async_error; // 应用可见普通资源失败。
    }
    root->async_owned = true; // cleanup和exit之后负责释放。
    place_frame(root); // 寄存器值进入本进程真实内核栈。
    root->state = USER_RUNNABLE; // 发布首次运行资格。
    root_pid = root->pid; // 会话根身份不会被孩子exit覆盖。
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
                if (processes[(slot(current) + n) % PROCESS_COUNT].state == USER_RUNNABLE) { // 等待者可能刚被唤醒。
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
    *result = (struct user_result){(int)code, root->user_ticks, timer_switches, wait_blocks, root->fault_error, io_block_count, io_wake_count, idle_halt_count}; // 回收前复制必要数值。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 当前不在任何用户内核栈上。
        if (processes[i].state != USER_UNUSED) // wait已回收槽不重复处理。
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
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
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    airi_files_init(); // 用户会话开始前创建只读seed与空RAM目录，仅启动一次。
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
        airi_console_print(console, "programs: hello processes worker broken files storage recover pipes waits async async-reset"); // broken是明确损坏的演示输入。
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
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
        airi_console_print(console, "cannot load %s: error=%d", name, error); // 串口详细日志还给出解析原因。
    else // 程序可以合法返回非零，例如worker的23。
        airi_console_print(console, "program %s exited %d", name, result.code); // 不把应用结果当成内核停止。
    return true; // GUI是否存在都不影响服务执行。
}
```

<a id="async-user-program"></a>
## 正常、受限设备与新映像分别验证

async先申请四个注册对象并验证第五个−28，再释放spare验证旧handle失效；坏提交指针−14不锁reader。fork孩子用父handle读得到−9，再在约十个全局tick的窗口内反复取得CPU工作，中途打印实际进展；这不是保证十tick都独占CPU。父提交写cookie30、FLUSH cookie10、读cookie20和越界cookie40，未收集时第五个提交因完成信用返回−11，buffer仍BUSY。

父每次next可在空CQ真正阻塞，也可立即取得快设备已完成的记录。按cookie而非位置核对，正常设备读回512字节模式；只读写CQ为−30，读和FLUSH仍须正常；已隔离设备有效I/O以CQ−5返回，越界仍CQ−22。受限配置明确结束错误路径，不伪装完整正常生命周期。

正常分支再启动读请求孩子并退出；父在复用槽的新buffer写parent-new，提交FLUSH并收集。单硬件消费者必须先收集旧ACTIVE才开始这个FLUSH，因此随后验证新内容不变时，旧设备借用已结束。另一个孩子发读后exec async-reset，新程序确认注册集合和CQ为空，写new-image并等retired_active归零，再确认内容未被旧完成覆盖。设备很快时关闭可能发生在完成后，轨迹会准确显示实际active或completed，不强迫固定时序。

最后根进程提交读和FLUSH却不收集即退出；内核根据当时阶段cancel或drop，再排空并核对池。默认实测中观察到退出孩子和exec孩子都曾active=1，随后出现各自旧epoch的drop；这不是所有机器每次都必须重复的时间保证。

**新建 `labs/airios/async.c`。** 先strlen再比较async-reset，避免对短argv固定读更长字面量而跨出用户栈页。

<!-- write: labs/airios/async.c -->
```c
#include <airi_libc.h> // 真正ring3用户通过自有ABI注册、提交、等待与退出。
static const uint8_t forbidden[32]; // 只读ELF区，完整CQ长度不允许内核写入。
static long submit(unsigned operation, uint64_t handle, uint64_t sector, uint64_t cookie) { // 本节小包装让调用点显示请求身份。
    struct airi_io_submission submission = {sector, cookie}; // 用户栈结构在本次调用里存在。
    return airi_io_submit(operation, handle, &submission); // 内核返回前已复制，不在设备完成前借这个局部地址。
}
static int await_started(void) { // 生命周期演示尽量在真实ACTIVE或已形成CQ后执行退出/exec。
    uint64_t deadline = airi_ticks() + 300; // 有上界，不让演示永久等状态。
    struct airi_io_state state; // 快照只包含当前轮次数值。
    do { // 设备可能很快，completed也说明请求已被服务。
        if (airi_io_state_get(&state)) // 状态错误不当作尚未开始。
            return 0; // 调用者以错误退出。
        if (state.active || state.completed) // 不强迫所有时序都观察到同一在途瞬间。
            return 1; // 关闭日志会记录真实阶段。
        airi_yield(); // 让其他用户运行，PIT会安排普通内核服务。
    } while (airi_ticks() < deadline); // 截止按真实tick。
    return 0; // 源码或设备无法推进，不能假装成功。
}
static int after_exec(void) { // 同一PID的新映像，不继承注册buffer或旧CQ。
    struct airi_io_state state; // 包含本PID旧epoch尚在收集的ACTIVE计数。
    struct airi_io_completion completion; // CQ只属于新epoch。
    if (airi_io_state_get(&state) || state.registered_buffers || state.outstanding || state.completed || airi_io_next(&completion, 0) != -11) // 旧资源不能被新程序收集。
        return 140; // 父wait可见错误。
    long buffer = airi_io_alloc(); // 可能复用旧buffer槽，但新handle与epoch独立。
    if (buffer < 0 || airi_io_put((uint64_t)buffer, "new-image", 9)) // 新数据不能被晚完成覆盖。
        return 141; // 不使用无效句柄。
    uint64_t deadline = airi_ticks() + 300; // 旧后端最迟按两秒驱动截止结束。
    do { // 旧请求若已完成，retired_active初始即可为0。
        if (airi_io_state_get(&state)) // 不能忽略状态错误。
            return 142; // 明确失败。
        if (!state.retired_active) // 同PID旧epoch的设备借用都已结束。
            break; // 无需固定睡一段时间猜结果。
        airi_yield(); // 内核继续pump并按旧epoch丢弃。
    } while (airi_ticks() < deadline); // 不无限等待。
    char data[9]; // 用户自己的输出数组。
    if (state.retired_active || airi_io_next(&completion, 0) != -11 || airi_io_get((uint64_t)buffer, data, sizeof(data)) || memcmp(data, "new-image", 9) || airi_io_free((uint64_t)buffer)) // 新CQ和新数据都不受旧完成影响。
        return 143; // 不是只看exec返回位置。
    airi_puts("[ASYNC_EXEC_RESET_OK] old_epoch=retired new_buffer=unchanged cq=empty\n"); // 是否实际DROP由内核轨迹另记。
    return 0; // 仍以父等待的同一PID结束。
}
int main(int argc, char **argv) { // 正常工作负载与exec目标共用ELF，明确按名字分支。
    if (argc != 1) // 当前自有启动参数固定。
        return 120; // 不读取不存在参数。
    size_t name_bytes = strlen(argv[0]); // 先找NUL，不能对短名字固定比较更长字符串跨越栈页。
    if (name_bytes == 11 && !memcmp(argv[0], "async-reset", 11)) // 新映像入口，静态数据已重新初始化。
        return after_exec(); // 旧C调用链不能回来。
    if (name_bytes != 5 || memcmp(argv[0], "async", 5)) // 只接受明确的正常名字。
        return 121; // 不猜测其他模式。
    long writer = airi_io_alloc(), reader = airi_io_alloc(), error_buffer = airi_io_alloc(), spare = airi_io_alloc(); // 当前owner最多四个注册对象。
    if (writer < 0 || reader < 0 || error_buffer < 0 || spare < 0 || airi_io_alloc() != -28) // 容量错误不能偷偷借其他owner预算。
        return 122; // 尚未提交，exit统一回收。
    uint8_t pattern[512], output[512]; // 用户指针从不直接进入DMA描述符。
    for (unsigned i = 0; i < 512; ++i) // 每个位置可辨认实际往返。
        pattern[i] = (uint8_t)(i * 17u + 3u); // 固定全扇区模式。
    if (airi_io_free((uint64_t)spare) || airi_io_get((uint64_t)spare, output, 1) != -9 || airi_io_put((uint64_t)writer, pattern, 512)) // 释放句柄不再可用，IDLE输入可复制。
        return 123; // 不把句柄当地址解引用。
    if (airi_io_submit(AIRI_IO_READ, (uint64_t)reader, (const struct airi_io_submission *)0xffffffff80000000ull) != -14) // 坏提交地址在接受前拒绝，不锁buffer。
        return 124; // 正常reader仍可提交。
    long child = airi_fork(); // 子有空异步上下文，用户栈里的handle整数仍复制。
    if (child < 0) // 明确进程资源错误。
        return 125; // 不等待不存在孩子。
    if (!child) { // 证明fork不自动赋予父注册buffer的所有权。
        if (airi_io_get((uint64_t)writer, output, 1) != -9) // 子PID无法使用父句柄。
            return 126; // 不能跨进程读内核buffer。
        airi_puts("[ASYNC_OTHER_TASK] inherited_handle=-9 cpu=running\n"); // 可在父CQ等待期间观察另一用户执行。
        uint64_t start = airi_ticks(), until = start + 10; // 约100毫秒CPU工作，时间来自PIT。
        unsigned reported = 0; // 中途记录一次实际取得CPU的进展。
        while (airi_ticks() < until) { // 这是对照工作负载，不是父亲循环重试CQ。
            if (!reported && airi_ticks() - start >= 5) { // 约50毫秒后仍在执行另一个用户任务。
                airi_puts("[ASYNC_OTHER_PROGRESS] cpu=continued\n"); // 节流测试将核对此时父亲仍在同一次CQ等待。
                reported = 1; // 不用大量日志制造人为延迟。
            }
            __asm__ volatile("pause"); // 后端推进由周期内核服务完成。
        }
        return 0; // 父之后wait回收。
    }
    if (submit(AIRI_IO_WRITE, (uint64_t)writer, 256, 30) || submit(AIRI_IO_FLUSH, 0, 0, 10) || submit(AIRI_IO_READ, (uint64_t)reader, 256, 20) || submit(AIRI_IO_READ, (uint64_t)error_buffer, 16, 40)) // 接受四条，最后一条由CQ报告范围错误。
        return 127; // 提交成功不当作I/O成功。
    if (submit(AIRI_IO_FLUSH, 0, 0, 50) != -11 || airi_io_free((uint64_t)writer) != -16 || airi_io_put((uint64_t)writer, pattern, 1) != -16 || airi_io_get((uint64_t)reader, output, 1) != -16) // 未收集CQ就仍占信用和BUSY。
        return 128; // 即使设备已快完成，也不可提前复用。
    struct airi_io_state state; // 只读观察，不消费CQ。
    if (airi_io_next((struct airi_io_completion *)forbidden, 0) != -14 || airi_io_state_get(&state) || state.outstanding != 4) // 坏CQ输出必须在pop之前拒绝。
        return 129; // 不能丢一条完成或提前归还信用。
    struct airi_io_completion completion; // 每次收集实际32字节结果。
    unsigned seen = 0; // 按cookie辨认，不依赖返回位置。
    long write_result = -999, read_result = -999, flush_result = -999; // 设备受限时按实际错误分支。
    for (unsigned i = 0; i < 4; ++i) { // 每条已接受请求都必须可收集。
        if (airi_io_next(&completion, 1) || completion.reserved) // 空CQ在同一次调用里阻塞，返回0表示已复制一条。
            return 130; // I/O字节/错误在记录内部。
        if (completion.cookie == 30 && completion.buffer == (uint64_t)writer) { // 明确写请求身份。
            write_result = (long)completion.result; seen |= 1; // 保存实际结果，不从submit0推断。
        } else if (completion.cookie == 10 && !completion.buffer) { // FLUSH不含数据buffer。
            flush_result = (long)completion.result; seen |= 2; // 同步结果可以是0或具体设备错误。
        } else if (completion.cookie == 20 && completion.buffer == (uint64_t)reader) { // 读结果只写reader。
            read_result = (long)completion.result; seen |= 4; // 收集后reader才IDLE。
        } else if (completion.cookie == 40 && completion.buffer == (uint64_t)error_buffer && completion.result == -22) // 正式日志扇区被有界接口拒绝。
            seen |= 8; // 错误完成仍结束buffer借用。
        else // 不匹配或重复身份不能算完整结果集合。
            return 131; // 避免按第i个结果猜第i个请求。
    }
    if (seen != 15 || airi_io_next(&completion, 0) != -11) // 无丢失也无额外结果。
        return 132; // 空CQ错误与I/O错误分别处理。
    if (write_result == 512) { // 正常可写设备必须实际读回模式并完成同步。
        if (read_result != 512 || flush_result || airi_io_get((uint64_t)reader, output, 512) || memcmp(output, pattern, 512)) // 不只核对完成数量。
            return 133; // 设备内容必须相符。
    } else if (write_result == -30) { // 真实只读特征，read和FLUSH仍可用。
        if (read_result != 512 || flush_result) // 限制不能掩盖其他设备错误。
            return 134; // 保留实际失败。
    } else if (write_result != -5 || read_result != -5 || flush_result != -5) // 已隔离设备以错误CQ交回，不伪造成功。
        return 135; // 未认识的错误不宣称通过。
    if (airi_io_free((uint64_t)writer) || airi_io_free((uint64_t)reader) || airi_io_free((uint64_t)error_buffer)) // 完成收集后才可释放。
        return 136; // 所有注册对象归还。
    int status = -1; // 自有wait普通退出码。
    if (airi_waitpid(child, &status) != child || status) // 另一个用户也必须正常结束。
        return 137; // 不留下工作负载身份。
    if (write_result != 512) { // 故障配置到这里已验证了完整错误结果与资源路径。
        airi_puts(write_result == -30 ? "[ASYNC_READONLY_OK] write=-30 read=512 cq=collected\n" : "[ASYNC_UNAVAILABLE_OK] valid_io=-5 cq=collected\n"); // 不冒充正常写读生命周期演示。
        return 0; // 内核与串口继续可用。
    }
    child = airi_fork(); // 接下来验证退出时仍有已接受请求。
    if (child < 0) // 不忽略容量失败。
        return 144; // 根进程退出仍由内核清理。
    if (!child) { // 子注册对象完全独立。
        long buffer = airi_io_alloc(); // 用自己的owner取得句柄。
        if (buffer < 0 || submit(AIRI_IO_READ, (uint64_t)buffer, 256, 70) || !await_started()) // 等到真实开始或完成，再离开身份。
            return 145; // 不用空队列假装在途退出。
        return 0; // exit撤销QUEUED/CQ，ACTIVE留内核收集，阶段见真实轨迹。
    }
    if (airi_waitpid(child, &status) != child || status) // 父取得退出状态不等于设备已结束。
        return 146; // 不隐藏孩子错误。
    long replacement = airi_io_alloc(); // 复用注册池槽的新owner对象。
    if (replacement < 0 || airi_io_put((uint64_t)replacement, "parent-new", 10) || submit(AIRI_IO_FLUSH, 0, 0, 80) || airi_io_next(&completion, 1) || completion.cookie != 80 || completion.result) // 单设备先收集旧ACTIVE再开始此FLUSH。
        return 147; // FLUSH完成之后旧设备借用必已结束。
    if (airi_io_get((uint64_t)replacement, output, 10) || memcmp(output, "parent-new", 10) || airi_io_free((uint64_t)replacement)) // 晚完成没有覆盖新owner内容。
        return 148; // 实际设备结果与代数身份一起验证。
    child = airi_fork(); // 另一个孩子以同PID换映像。
    if (child < 0) // 不等不存在进程。
        return 149; // 资源由根退出清理。
    if (!child) { // 新映像会检查空注册集合并等待退役ACTIVE。
        long buffer = airi_io_alloc(); // 当前孩子的旧轮次资源。
        if (buffer < 0 || submit(AIRI_IO_READ, (uint64_t)buffer, 256, 90) || !await_started()) // 后端确已推进这条请求。
            return 150; // 不把纯提交当成晚完成实测。
        airi_exec("async-reset"); // 成功保留PID，异步owner使用新epoch。
        return 151; // 成功不能回旧用户调用链。
    }
    if (airi_waitpid(child, &status) != child || status) // 同PID的新程序仍由父正常回收。
        return 152; // 新程序验证失败不能吞掉。
    long last = airi_io_alloc(); // 最后演示不收CQ直接退出的清理。
    if (last < 0 || submit(AIRI_IO_READ, (uint64_t)last, 256, 100) || submit(AIRI_IO_FLUSH, 0, 0, 101)) // 状态可能排队或已在途，以内核轨迹为准。
        return 153; // 不接受半个退出演示。
    airi_puts("[ASYNC_USER_OK] cookies=matched buffers=protected fork=private exec=reset exit=pending\n"); // 内核会在退出后继续排空并核对静态池。
    return 0; // 不提前释放last，exit负责取消/隔离/最终收集。
}
```

**更新用户声明。** 公开io_abi可包含，内核池结构不可见；下一块Make会加入其头文件路径，完成本节再构建。

<!-- write: labs/airios/include/airi_libc.h -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
#include <airi/io_abi.h> // 自有固定宽度提交、完成和只读状态，不包含内核结构。
#include <stddef.h> // 用户对象字节长度。
#include <stdint.h> // 用户ABI的64位寄存器值。
void *memcpy(void *destination, const void *source, size_t bytes); // 双方不重叠的用户空间复制。
void *memmove(void *destination, const void *source, size_t bytes); // 允许同一用户数组内重叠移动。
void *memset(void *destination, int value, size_t bytes); // 按字节填充用户对象。
int memcmp(const void *left, const void *right, size_t bytes); // 首个不同无符号字节决定结果。
size_t strlen(const char *text); // 用户有效NUL字符串长度。
long airi_write(unsigned fd, const void *buffer, size_t bytes); // 自有int80 ABI，失败直接返回负数。
long airi_getpid(void); // 获取当前用户身份。
_Noreturn void airi_exit(unsigned code); // 退出不返回用户调用方。
long airi_puts(const char *text); // 有界串口write，不自动补换行。
long airi_fork(void); // 首版内核直接复制用户页，子返回0，父返回子PID。
long airi_waitpid(long pid, int *status); // pid=-1或指定孩子；status是普通退出值。
void *airi_alloc(size_t bytes); // 零填充页申请，失败NULL，进程结束统一回收。
uint64_t airi_ticks(void); // 实际时钟事件计数。
void airi_yield(void); // 主动交接，不等同于时钟抢占。
long airi_exec(const char *name); // 成功替换映像不返回；失败保留旧PID和调用链，返回负错误。
#define AIRI_READ 1u // 以下标志只属于自有open ABI。
#define AIRI_WRITE 2u // 写访问。
#define AIRI_CREATE 4u // 名字不存在时创建。
#define AIRI_TRUNCATE 8u // 清空可写文件。
#define AIRI_CLOEXEC 16u // 成功exec关闭此表项。
long airi_open(const char *path, unsigned flags); // 平坦绝对名字，返回FD或负错误。
long airi_close(unsigned fd); // 关闭不删除名字。
long airi_dup(unsigned fd, unsigned cloexec); // 与源共享偏移。
long airi_seek(unsigned fd, uint64_t offset); // 只支持绝对定位。
long airi_read(unsigned fd, void *buffer, size_t bytes); // EOF为0，短读是实际完成数。
long airi_pread(unsigned fd, void *buffer, size_t bytes, uint64_t offset); // 不移动共享偏移。
long airi_pwrite(unsigned fd, const void *buffer, size_t bytes, uint64_t offset); // 第四参数R10。
long airi_log_append(const void *data, size_t bytes); // 最多480字节，成功返回从1开始的sequence。
long airi_log_read(unsigned index, void *data, size_t capacity); // 索引从0，返回实际长度。
long airi_log_count(void); // 已恢复有效前缀，错误为负。
#define AIRI_READABLE 1u // 当前read可得到数据或EOF。
#define AIRI_WRITABLE 2u // 当前有读者和空位。
#define AIRI_HANGUP 4u // 最后写者关闭，仍应排空已有数据。
#define AIRI_ERROR 8u // 最后读者关闭，写将EPIPE。
long airi_pipe(unsigned ends[2], unsigned nonblocking); // nonblocking=0阻塞，1暂不可完成时返回-11。
long airi_ready(unsigned fd); // 管道当前条件查询，不是Linux epoll ABI。
long airi_wait_ready(unsigned fd, unsigned events, unsigned ticks); // READ/WRITE条件，0纯查询，最多500tick，超时返回0。
long airi_io_alloc(void); // 注册一个512字节内核buffer，返回句柄而不是指针。
long airi_io_put(uint64_t handle, const void *data, size_t bytes); // IDLE可修改，BUSY返回-16。
long airi_io_get(uint64_t handle, void *data, size_t bytes); // CQ收集后才可读取。
long airi_io_free(uint64_t handle); // 旧句柄-9，BUSY-16。
long airi_io_submit(unsigned operation, uint64_t handle, const struct airi_io_submission *submission); // 0仅接收，描述在返回前已复制。
long airi_io_next(struct airi_io_completion *completion, unsigned wait); // 0不等，1阻塞；返回0仅表示交出一条CQ。
long airi_io_state_get(struct airi_io_state *state); // 当前轮次与同PID退役ACTIVE的只读数值。
#endif // 结束包含保护。
```

**更新自有包装。** 提交描述在一次int80内复制，CQ输出按固定长度验证。

<!-- write: labs/airios/libc/syscall.c -->
```c
#include <airi_libc.h> // 自有用户接口，没有Linux系统调用编号。
static long call3(long number, uint64_t first, uint64_t second, uint64_t third) { // 统一约定RAX/RDI/RSI/RDX。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内核恢复其他通用寄存器，RAX改为结果。
    return number; // +a表示同一个C对象既提供输入又接收输出。
}
long airi_write(unsigned fd, const void *buffer, size_t bytes) { // 用户指针由内核完整检查，库不伪造成功。
    return call3(1, fd, (uintptr_t)buffer, bytes); // 负值直接交给本节调用者，尚无errno变量。
}
long airi_getpid(void) { // 不需要参数的系统调用仍使用同一寄存器入口。
    return call3(3, 0, 0, 0); // 返回当前用户身份。
}
_Noreturn void airi_exit(unsigned code) { // 本函数成功时不返回。
    call3(2, code, 0, 0); // 内核选择启动任务现场继续。
    for (;;) // 若内核错误地返回这里，产生明确用户异常。
        __asm__ volatile("ud2"); // 不在未知退出状态下继续执行。
}
long airi_puts(const char *text) { // 文本长度先在当前用户地址空间计算。
    return airi_write(1, text, strlen(text)); // 当前内核最大256字节，过长会返回-EINVAL。
}
long airi_fork(void) { // 用户库不复制内存，交给内核建立子根和返回现场。
    return call3(6, 0, 0, 0); // 正PID/0/负错误保留原样。
}
long airi_waitpid(long pid, int *status) { // status为自有普通int退出码，不是Linux wait编码。
    return call3(8, (uint64_t)pid, (uintptr_t)status, 0); // 父进程可阻塞到这个调用的返回点。
}
void *airi_alloc(size_t bytes) { // 本节只按页申请，不提供逐对象free。
    long result = call3(9, bytes, 0, 0); // 内核完整成功后才前移用户VA游标。
    return result < 0 ? NULL : (void *)(uintptr_t)result; // 有界页接口以NULL表示申请失败，尚无errno。
}
uint64_t airi_ticks(void) { // 用来观察真实用户时间片。
    return (uint64_t)call3(5, 0, 0, 0); // 不把固定空循环步数当作时钟。
}
void airi_yield(void) { // 可主动让出，但抢占工作负载不使用它。
    call3(4, 0, 0, 0); // 由内核在完整陷入现场选择其他用户。
}
long airi_exec(const char *name) { // 成功进入新_start，只有失败才返回这条旧调用链。
    return call3(7, (uintptr_t)name, 0, 0); // 当前只传程序名，argv由内核建立单项。
}

static long call4(long number, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth) { // 前三参数与call3相同，显式偏移需要第四槽。
    register uint64_t r10 __asm__("r10") = fourth; // C寄存器变量固定到自有ABI的R10。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third), "r"(r10) : "memory", "cc"); // 陷入现场完整保存R10。
    return number; // 负错误保持原样。
}
long airi_open(const char *path, unsigned flags) { // 路径由内核有界复制。
    return call3(11, (uintptr_t)path, flags, 0); // FD是当前进程的编号。
}
long airi_close(unsigned fd) { // 释放一个表项引用。
    return call3(12, fd, 0, 0); // 已关闭再关闭为-9。
}
long airi_dup(unsigned fd, unsigned cloexec) { // 不重新open文件。
    return call3(13, fd, cloexec, 0); // 新表项的标志独立。
}
long airi_seek(unsigned fd, uint64_t offset) { // 自有接口没有whence参数。
    return call3(14, fd, offset, 0); // 返回绝对偏移。
}
long airi_read(unsigned fd, void *buffer, size_t bytes) { // 同步读至当前用户可写范围。
    return call3(10, fd, (uintptr_t)buffer, bytes); // 返回数不能直接假定等于bytes。
}
long airi_pread(unsigned fd, void *buffer, size_t bytes, uint64_t offset) { // 显式偏移不修改共享打开位置。
    return call4(15, fd, (uintptr_t)buffer, bytes, offset); // R10传偏移，不借用用户栈参数布局。
}
long airi_pwrite(unsigned fd, const void *buffer, size_t bytes, uint64_t offset) { // 与pread使用同一第四参数约定。
    return call4(16, fd, (uintptr_t)buffer, bytes, offset); // 写也可能短于请求。
}

long airi_log_append(const void *data, size_t bytes) { // 同步记录提交，不等同普通FD写。
    return call3(17, (uintptr_t)data, bytes, 0); // 数据和commit同步后才返回sequence。
}
long airi_log_read(unsigned index, void *data, size_t capacity) { // 内核验证完整输出范围。
    return call3(18, index, (uintptr_t)data, capacity); // 小容量返回错误，不部分截断记录。
}
long airi_log_count(void) { // 只返回数值，不借内核对象。
    return call3(19, 0, 0, 0); // 真实恢复失败保持负错误。
}

long airi_pipe(unsigned ends[2], unsigned nonblocking) { // 内核先验证输出再原子创建两个FD。
    return call3(20, (uintptr_t)ends, nonblocking, 0); // 成功后ends[0]读、ends[1]写。
}
long airi_ready(unsigned fd) { // 仅查询当前管道条件。
    return call3(21, fd, 0, 0); // 返回掩码或负错误，不能把负数当事件位。
}

long airi_wait_ready(unsigned fd, unsigned events, unsigned ticks) { // 条件满足返回事件位，超时0，负值是错误。
    return call3(22, fd, events, ticks); // 内核保存现场并在同一次调用后恢复。
}

long airi_io_alloc(void) { // 自有句柄不是可解引用的用户内存。
    return call3(23, 0, 0, 0); // 最多四个注册buffer。
}
long airi_io_put(uint64_t handle, const void *data, size_t bytes) { // 用户输入先同步复制，之后不能覆盖BUSY。
    return call3(24, handle, (uintptr_t)data, bytes); // 最多512字节，短输入尾部清零。
}
long airi_io_get(uint64_t handle, void *data, size_t bytes) { // 完整输出范围在内核验证。
    return call3(25, handle, (uintptr_t)data, bytes); // 成功0，字节范围由本次bytes决定。
}
long airi_io_free(uint64_t handle) { // 注册资源与用户堆页分开。
    return call3(26, handle, 0, 0); // 不允许提前释放请求借用的buffer。
}
long airi_io_submit(unsigned operation, uint64_t handle, const struct airi_io_submission *submission) { // 本次int80内复制16字节数值结构。
    return call3(27, operation, handle, (uintptr_t)submission); // 成功不代表I/O已经完成。
}
long airi_io_next(struct airi_io_completion *completion, unsigned wait) { // 空CQ可返回-11或登记当前用户等待。
    return call3(28, (uintptr_t)completion, wait, 0); // 真正I/O字节/错误保存在completion.result。
}
long airi_io_state_get(struct airi_io_state *state) { // 状态快照不会消耗CQ或解锁buffer。
    return call3(29, (uintptr_t)state, 0, 0); // 四十字节，不含内核地址。
}
```

**更新映像包装。** async与async-reset通过同一个静态ELF的argv明确分支。

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
```

**更新编译。** USER_FLAGS加入公开include路径，用户对象显式依赖io_abi.h，协议变化不能复用旧对象；不把内核内部结构当用户ABI。

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
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户独立静态ELF是嵌入输入，不是内核的直接链接对象。
$(OUT)/user/hello.elf: $(USER_OBJECTS) labs/airios/program.ld
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf $(OUT)/user/waits.elf $(OUT)/user/async.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -DAIRI_STORAGE_ELF='"$(OUT)/user/storage.elf"' -DAIRI_PIPES_ELF='"$(OUT)/user/pipes.elf"' -DAIRI_WAITS_ELF='"$(OUT)/user/waits.elf"' -DAIRI_ASYNC_ELF='"$(OUT)/user/async.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 多进程工作负载有自己的独立静态ELF。
$(OUT)/user/processes.elf: $(USER_COMMON) $(OUT)/user/processes.o labs/airios/program.ld
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# worker也有自己的_start和main，原始ELF只在包装时进入内核。
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 只链接这份工作负载的main。
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# storage与recover由同一ELF按明确argv分支。
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 管道程序只复用自有用户入口与库。
$(OUT)/user/pipes.elf: $(USER_COMMON) $(OUT)/user/pipes.o labs/airios/program.ld
# 保持相同低地址ELF布局。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# ELF只选择waits的main。
$(OUT)/user/waits.elf: $(USER_COMMON) $(OUT)/user/waits.o labs/airios/program.ld
# 低地址静态链接，不借宿主入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@

# 异步用户程序与重置别名共用独立main和低地址入口。
$(OUT)/user/async.o: labs/airios/async.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 当前配置的对象目录。
	@mkdir -p $(dir $@)
# USER_FLAGS含公开ABI头，不能依赖内核内部结构。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 自有入口、库与async main单独链接。
$(OUT)/user/async.elf: $(USER_COMMON) $(OUT)/user/async.o labs/airios/program.ld
# 仍为无宿主libc的静态ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@
```

<a id="async-throttled-proof"></a>
## 用真实慢设备验证等待期间另一个用户继续

快设备可能在调用next前已有CQ，因此不能把“每次都应阻塞”写成验收。storage_test复用已有UART/QMP生命周期，在两个新CPU恢复同一日志之后，再启第三个guest；启动完成才用QMP block_set_io_throttle将独立airi-disk限制为1 IOPS。QEMU实际推迟设备I/O，guest没有为演示伪造延迟。

第三次run async的记录只消费该命令之后的新输出。程序第一次fork提供实际父PID，测试逐个寻找这个PID的CQ登记与对应WAKE，要求ASYNC_OTHER_PROGRESS出现在其中一个等待区间内，同时有真实start和complete轨迹。这样才证明另一个用户在父同一次CQ等待期间运行，不能仅凭QEMU仍存活或一行静态“并发”文字通过。

Python正则的r前缀保留反斜杠，\[匹配字面左方括号，(\d+)捕获一组十进制数字。例如本次命令若记录parent=29，group(1)取得字符串29；finditer逐个给出匹配区间，start/end是这次输出中的字符偏移。find从登记之后找同PID完整result=0加CRLF，缺失返回−1，无法满足中途进展位于两者之间的条件。所有范围只用本次命令之后的新文本。

限速只用于第三个新guest，前面的启动日志实验不受影响；每个实际块请求仍受首次begin的两秒截止保护。结果记录restart_boots=2、boots=3和other_user_during_cq_wait=true，原始限速参数只在QMP接受后写出。受控QEMU退出仍不冒充主机掉电，正式日志区与异步块区也始终分开。

**更新真实会话验证。** 双启动恢复保留，第三次单独证明慢设备CQ等待和其他用户进展。

<!-- write: scripts/storage_test.py -->
```python
"""Run two actual QEMU boots against one dedicated, freshly created test disk.""" # 跨VM启动与模型crash分别验证。
import re # 从实际PID和完成记录关联等待区间，不固定猜PID。
import json # 记录本轮事实，不执行JSON内容。
from pathlib import Path # 构造项目内独立证据路径。
import socket # 实际UART收发。
import subprocess # 启动和回收真实QEMU。
import tempfile # 短端点路径避免macOS UNIX长度上限。
import time # 单调截止时间不受墙钟变化影响。
from boot import command, qmp_execute # 固定硬件与实际QMP继续。
from tools import ROOT # 不依赖调用者当前目录。

def check_storage_restart(profile='normal'): # 对当前构建正常或图形正常配置运行两次。
    work = ROOT / 'artifacts' / ('storage-restart-' + profile) # 仅此测试拥有的实验盘。
    work.mkdir(parents=True, exist_ok=True) # 证据父目录必须存在。
    result_path = work / 'result.json' # 本轮状态路径。
    result_path.write_text(json.dumps({'passed':False, 'status':'running'}) + '\n') # 旧成功不能覆盖新失败。
    (work / 'async-throttle.json').unlink(missing_ok=True) # 失败不能留下上次QMP已接受的身份。
    disk = work / 'disk.raw' # 此测试每次从空盘开始，普通run目录不受影响。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024)): # 未知对象不能当测试盘删除。
        raise RuntimeError('Invalid restart-test disk; refusing to replace') # 明确保留异常路径。
    disk.unlink(missing_ok=True) # 只删除本测试上次生成的8MiB盘，下一command排他创建新盘。
    started = time.monotonic() # 整组测试耗时。
    def boot_once(label, kernel_marker, request, user_marker, slow=False): # 同一work保存盘，端点和CPU每次新建。
        with tempfile.TemporaryDirectory(prefix='airios-store-', dir='/tmp') as directory: # 短临时路径仅放通信端点。
            qmp = Path(directory) / 'qmp.sock' # QMP不是设备输出。
            uart = Path(directory) / 'uart.sock' # UART在CPU继续前连接。
            cmd = command(profile, work) # 复制本轮固件变量，保持同一个disk.raw。
            cmd[cmd.index('-serial') + 1] = f'unix:{uart},server=on,wait=off' # 串口改成真实收发端点。
            cmd += ['-S', '-qmp', f'unix:{qmp},server=on,wait=off'] # 先暂停，接收端准备好才继续。
            serial = work / (label + '.log') # 每个新CPU单独记录，不能读取上次READY。
            data = bytearray() # 只累积本次设备字节。
            with (work / (label + '-qemu.log')).open('w') as errors: # QEMU自身诊断与内核输出分开。
                process = subprocess.Popen(cmd, stdout=errors, stderr=errors) # 启动真实独立进程。
                wire = None # 连接失败也进入finally清理。
                try: # 每个失败都必须收回QEMU。
                    deadline = time.monotonic() + 10 # 端点等待另有总时间上限。
                    while not uart.exists(): # 等待服务端创建端点。
                        if process.poll() is not None or time.monotonic() > deadline: # 已退出或耗时过长。
                            raise RuntimeError('Restart-test UART unavailable') # 不能无限等。
                        time.sleep(.05) # 给QEMU建立端点的时间。
                    wire = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) # 本机UART流，不走网络服务。
                    wire.connect(str(uart)) # CPU仍暂停，早期输出不会丢失。
                    wire.setblocking(False) # 暂无字节不阻塞整个控制循环。
                    qmp_execute(qmp, 'cont') # 接收端存在后继续CPU。
                    def output(): # 累积本次所有已到设备字节。
                        while True: # 取到当前无更多数据为止。
                            try: # 非阻塞接收暂时无数据是正常情况。
                                chunk = wire.recv(4096) # 分块读，不假定一条消息一次到齐。
                            except BlockingIOError: # 稍后再试。
                                break # 本轮读完。
                            if not chunk: # 设备流关闭。
                                break # 后续进程状态给出失败。
                            data.extend(chunk) # 保留原字节顺序。
                        serial.write_bytes(data) # 失败前也保存已收到证据。
                        return data.decode(errors='replace') # 不因单个解码问题丢整份日志。
                    def wait(marker, offset=0): # 只在本次请求后的完整行内找标记。
                        deadline = time.monotonic() + 35 # 真实目标标记最多等待35秒。
                        while time.monotonic() < deadline: # 使用单调截止时间。
                            text = output() # 读取本次设备输出。
                            for line in text[offset:].splitlines(keepends=True): # 保留结束符以拒绝半行。
                                record = line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r') # 只移除已知提示符与CRLF。
                                if line.endswith('\n') and (record == marker or (marker.endswith(' ') and record.startswith(marker))): # 完整等值或明确带空格字段边界，拒绝半行和相似命令计数。
                                    return text # 交回实际累计输出。
                            if process.poll() is not None: # 提前退出不能通过。
                                raise RuntimeError(f'Guest exited before {marker}') # 保留目标身份。
                            time.sleep(.05) # 等设备与CPU推进。
                        raise RuntimeError(f'Restart test timed out: {marker}; see {serial}') # 明确失败文件。
                    text = wait('[AIRI_P5_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    if slow: # 启动后才节流，前面的块/日志演示不受影响。
                        limits = {'device':'airi-disk', 'bps':0, 'bps_rd':0, 'bps_wr':0, 'iops':1, 'iops_rd':0, 'iops_wr':0} # 明确单IOPS且无额外突发，不限制吞吐字节。
                        qmp_execute(qmp, 'block_set_io_throttle', limits) # QEMU实际限速，不在guest里伪造完成延迟。
                        (work / 'async-throttle.json').write_text(json.dumps(limits, indent=2) + '\n') # 只在QMP接受后记录本次配置。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', offset) # 等命令与用户会话全部收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                    return text[offset:] # 交出本次请求的完整真实区间，finally仍先回收QEMU。
                finally: # 正常和错误均收回设备与CPU。
                    if wire is not None: # 建立过才关闭。
                        wire.close() # 结束本次UART连接。
                    process.terminate() # 这是受控进程结束，不冒充真实主机掉电。
                    try: # 请求结束还需确认进程已回收。
                        process.wait(timeout=5) # 清理也有截止时间。
                    except subprocess.TimeoutExpired: # QEMU未及时退出。
                        process.kill() # 强制结束本测试子进程。
                        process.wait() # 不遗留僵尸。
    boot_once('first', '[STORAGE_RECOVERED] count=0 truncated=0', 'run storage', '[STORAGE_USER_OK]') # 空盘上由用户实际追加并同步。
    boot_once('second', '[STORAGE_RECOVERED] count=1 truncated=0', 'run recover', '[RECOVERY_USER_OK]') # 新CPU和新内存从同一盘恢复。
    tail = boot_once('async', '[STORAGE_RECOVERED] count=1 truncated=0', 'run async', '[ASYNC_USER_OK]', slow=True) # 同一正式日志保留，第三个新CPU单独验证慢设备上的CQ等待。
    match = re.search(r'\[FORK\] parent=(\d+) child=(\d+)', tail) # 当前命令第一次fork给出真实根PID。
    assert match is not None # 不把启动时旧PID或固定常量混入本次证据。
    pid = match.group(1) # 字符串PID用于精确字段匹配。
    progress = tail.index('[ASYNC_OTHER_PROGRESS]') # 另一个真正用户取得CPU后的进展。
    blocked = False # 必须找到一次父CQ阻塞覆盖这条进展。
    for registered in re.finditer(r'\[WAIT_REGISTER\] pid=' + pid + r' operation=cq\r?\n', tail): # 每个候选都是本次父请求的实际登记。
        wake = tail.find('[WAIT_WAKE] pid=' + pid + ' result=0\r\n', registered.end()) # 找此登记之后同PID的结果交付。
        if registered.start() < progress < wake: # 子任务进展发生在同一次父调用等待期间。
            blocked = True # 不是凭QEMU仍运行推断并发。
    assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 真设备接收、完成和其他用户继续必须同时成立。
    result = {'passed':True, 'profile':profile, 'boots':3, 'restart_boots':2, 'recovered_records':1, 'throttled_async_iops':1, 'other_user_during_cq_wait':True, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage sessions: two-boot recovery + QMP-throttled CQ wait with other user progress') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
```

**更新入口说明。** 完整test继续调用同一验证，结果多记录一次真实guest。

<!-- write: scripts/manage.py -->
```python
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import json      # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex     # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import sys  # 取得当前解释器路径、入口参数及标准错误流。
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
    # GNU Make dry-run is the single source of compiler flags, including headers.
    command=make_command('normal','kernel')  # 局部 command 是 Make 参数列表，不是导入的QEMU command函数。
    command.insert(1,'-Bn')  # 在可执行路径之后插入 -B强制列规则、-n只打印的选项。
    lines=subprocess.check_output(command,cwd=ROOT,text=True).splitlines()  # 读取命令文本并按行拆分，不执行打印出的编译器命令。
    entries=[]  # 保存编译数据库条目，每项对应一份源文件及参数。
    for line in lines:  # 逐条检查 Make 打印的命令。
        if ' -c ' not in line or ' -o ' not in line: continue  # 过滤目录创建与链接命令，只考虑单文件编译。
        tokens=shlex.split(line)  # 按shell引号解析参数边界，列表值随后直接保存。
        if '-c' not in tokens or '-o' not in tokens: continue  # 分词后再确认编译选项，排除仅字符串碰巧匹配的行。
        source=tokens[tokens.index('-c')+1]  # index 找到-c位置，其后一个元素是被编译的源文件路径。
        entries.append({'directory':str(ROOT),'file':str(ROOT/source),'arguments':tokens})  # 保存工作目录、绝对源路径和完整参数列表供 clangd 消费。
    target=ROOT/'build/normal/compile_commands.json'  # 数据库放在普通配置目录，.clangd 从此处读取。
    target.parent.mkdir(parents=True,exist_ok=True)   # 数据库父目录缺失时创建，已有目录保持。
    target.write_text(json.dumps(entries,indent=2)+'\n')  # 列表编码为JSON数组，输出本次真实构建参数。

def build(profile):  # 给指定配置调用同一Make规则，普通配置额外更新编辑器数据库。
    run(make_command(profile))  # 组装配置及工具覆盖，执行完整内核与镜像构建。
    if profile=='normal':  # 测试配置不替换日常编辑参数。
        compilation_database()  # 普通配置才导出clangd数据库。
def inspect_elf(profile='normal'):  # 核对独立内核目标，不接受宿主解释器、动态依赖或未解析符号。
    binary=ROOT/'build'/profile/'airios.elf'  # 定位本次配置的ELF，避免检查另一目录的旧产物。
    readelf=tool('llvm-readelf','llvm')  # 按既定规则找到 LLVM ELF 检查器。
    report=subprocess.check_output([readelf,'-h','-l','-d','-s',str(binary)],text=True)  # 合并ELF头、程序段、动态信息和符号表的文本报告。
    nm=subprocess.check_output([tool('llvm-nm','llvm'),'-u',str(binary)],text=True)  # -u 只列未定义符号，独立内核应没有内容。
    if nm.strip() or 'INTERP' in report or '(NEEDED)' in report:  # 非空未定义项、解释器段或动态依赖任一存在就拒绝。
        raise RuntimeError('Kernel is not a fully resolved freestanding executable')  # 报告不满足完全解析的独立目标约定。
    if 'X86-64' not in report and 'x86-64' not in report:  # 兼顾工具文字大小写，必须出现x86-64机器类型。
        raise RuntimeError('Wrong ELF target')  # 错误机器目标不能作为此次内核成功结果。
    if 'EXEC (Executable file)' not in report:  # 要求固定地址静态ET_EXEC，与链接脚本装载方式一致。
        raise RuntimeError('Expected a static ET_EXEC ELF')  # PIE或其他ELF种类不满足目前启动路径。
    (ROOT/'artifacts').mkdir(exist_ok=True)  # 建立检查报告目录。
    (ROOT/'artifacts/kernel-elf.txt').write_text(report)  # 保存实际ELF检查输出，便于后续调试查看。
    print('PASS ELF: x86-64 ET_EXEC, no interpreter/dependencies/undefined symbols')  # 只在机器、类型、依赖和符号全部符合后打印通过。

def doctor():  # 查工具与版本，再实际准备和构建内核以验证环境。
    record={}  # 字典以工具名称为键，值保存路径和首行版本。
    for name,formula in [('clang','llvm'),('ld.lld','lld'),('qemu-system-x86_64','qemu'),  # 每项二元组给可执行名称与对应软件包标识。
                         ('xorriso','xorriso'),('gmake','make'),('llvm-readelf','llvm')]:  # 继续列镜像制作、GNU Make和ELF检查器；循环解包到name/formula。
        executable=make_command()[0] if name=='gmake' else tool(name,formula)  # GNU Make沿用gmake再make的选择，其余工具按既有规则查找。
        flag='-version' if name=='xorriso' else '--version'  # xorriso 使用单横线，其余工具使用双横线版本参数。
        result=subprocess.run([executable,flag],capture_output=True,text=True)  # 启动版本命令，分别收集标准输出与错误文本。
        version=(result.stdout+result.stderr).splitlines()[0]  # 版本可能写在任一流，取合并后的第一行。
        record[name]={'path':executable,'version':version}     # 工具键映射到实际路径和版本，不只有安装包名称。
        print(f'OK {name}: {version}')  # 显示本轮查到的工具身份和版本。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    run(make_command(target='kernel'))  # 通过真实内核编译和链接验证环境，不仅看工具是否存在。
    compilation_database()  # 使用同一规则生成编辑器参数。
    inspect_elf()  # 检查刚构建出的普通配置ELF。
    (ROOT/'artifacts/toolchain.json').write_text(json.dumps(record,indent=2)+'\n')  # 保存本次实际工具路径和版本。
def main():  # 解析统一入口动作，并把它交给先前实现的脚本。
    if len(sys.argv)>1 and sys.argv[1]=='learn':  # 先检查有动作参数，再识别采用独立子解析器的learn。
        from learning import main as learn  # 只进入learn时导入后续提供的学习模块，别名避免与当前main混淆。
        learn(sys.argv[2:])  # 切片丢掉程序名和learn，把余下参数交给子入口。
        return  # 当前动作完成，不继续后面的构建或分派。
    parser=argparse.ArgumentParser(description='AiriOS build and learning tools')  # 建立动作解析器，不合法参数由argparse给出用法。
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
    if args.action=='snapshots':  # 只有导出动作需要读取Git教学提交链。
        from snapshots import generate  # 局部导入后续提供的快照模块。
        generate()  # 核对固定main提交链和旧目录保护后导出阶段源码。
        return      # 当前动作完成，不继续后面的构建或分派。
    if args.action=='docs-check':  # 只检查重建与导航时无需启动QEMU。
        from docs_check import check_docs  # 只有明确来源检查才读取全部课程。
        check_docs()  # 核对正文重建与本地导航，不阻塞默认 test。
        return  # 当前动作完成，不继续后面的构建或分派。
    if args.graphics:  # G24已提供纯标准库资产检查，运行图形不依赖G26的截图工具。
        from graphics_assets import verify_fonts  # 显式选择后才读取图形目录。
        verify_fonts()  # 不导入Pillow或Cocoa。
    if args.action in ('ui-preview', 'ui-test') or (args.action == 'test' and args.graphics):  # 完整图形回归在G26后使用。
        from ui_preview import preview, check_frontend  # 主线不会导入这份可选工具。
    if args.action=='ui-preview':  # 主机窗口无需内核启动依赖。
        preview(args.app,args.state)  # 打开指定应用和服务状态。
        return  # 此动作已完成。
    if args.action=='ui-test':  # 主机行为与图像观察采用前章接口。
        check_frontend()  # 运行共享C测试与平台可用快照。
        return  # 此动作已完成。
    if args.action=='clean':  # 清理不需要再构建内核。
        run(make_command(target='clean'))  # 调用Make既有删除规则。
        return  # 清理结束。
    if args.action=='linux-lab':  # Linux对照程序由独立脚本传到个人主机运行。
        run([sys.executable,str(ROOT/'scripts/linux_lab.py'),'--host',args.host,'--phase',args.phase])  # 当前解释器执行个人主机实验。
        return  # 远程实验结束后不启动另一台虚拟机。
    if args.action=='doctor':  # doctor自己按需准备和构建。
        doctor()  # 查工具、构建、生成编辑器参数并检查ELF。
        return    # 环境检查结束。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    if args.graphics:  # 图形和串口使用不同产物目录，不复用旧目标。
        args.profile = 'desktop-' + args.profile  # 如 normal 变为 desktop-normal。
    if args.action=='kernel-debug':  # 内核GDB观察需要先得到普通配置带符号ELF。
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、九种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定九种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 九种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
            work=ROOT/'artifacts'/('interactive-'+args.profile)  # 交互日志按配置分开保存。
            if args.action=='debug':  # debug把CPU停在复位并打开回环GDB端点。
                print('QEMU waits at reset. Remote GDB: target remote localhost:1234\n'  # 告诉调试者当前CPU尚未前进及连接地址。
                      'Then hbreak _start; continue. See dev-log/linux/01-entry.md.',flush=True)  # 提示入口硬件断点，flush保证运行虚拟机前已打印。
            if args.graphics:  # 图形交互仍将UART写入本轮文件。
                print(f'Serial log: {work/"serial.log"}',flush=True)  # 显示本次实际串口日志路径，立即刷新输出。
            cmd = command(args.profile,work,gui=args.graphics,debug=args.action=='debug')  # 先形成具体 QEMU 参数。
            if not args.graphics:  # 串口交互直接连接当前主机终端。
                cmd[cmd.index('-serial')+1] = 'mon:stdio'  # QEMU 用 Ctrl-A X 退出，Ctrl-A C 切监视器。
                print('Serial console: this terminal. Exit QEMU with Ctrl-A X.', flush=True)  # 字节不再写入文件。
            run(cmd)  # 启动所选串口或图形QEMU，debug动作才带-S和GDB。

if __name__=='__main__':  # 直接执行脚本才启动CLI，导入时仅定义接口。
    try:  # 入口错误统一转成明确的失败状态。
        main()  # 执行解析和分派，异常留给下面处理。
    except (RuntimeError,subprocess.CalledProcessError,AssertionError) as error:  # 三类异常都保留具体原因并转成非零退出。
        print(f'ERROR: {error}',file=sys.stderr)  # 错误文字送标准错误，不混入正常结果流。
        sys.exit(1)  # 失败状态交给shell和调用脚本，不能继续宣称通过。
```

**更新初始化与P5标记。** 可写正常设备完成用户生命周期后才发布AIRI_P5_READY；Linux结果另记。

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
    airi_async_init(); // 所有设备配置都初始化固定资源池，设备错误由CQ表达。
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
    if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
        airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
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

**更新启动预期。** 正常等P5，只读只宣称P4可读能力，隔离设备保留P3并核对错误CQ。

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
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' if case == 'block-readonly' else '[AIRI_P5_READY]' # 正常等完整异步阶段，受限设备只发布实际可用能力。
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
                        user_async_marker = '[ASYNC_UNAVAILABLE_OK]' if case == 'block-timeout' else '[ASYNC_READONLY_OK]' if case == 'block-readonly' else '[ASYNC_USER_OK]' # 真正用户程序按设备状态分支。
                        assert user_async_marker in text and '[ASYNC_USER_DEMO_OK]' in text # 资源回收由launch核对。
                        if case not in ('block-timeout', 'block-readonly'): # 可写正常设备还须通过新epoch验证。
                            assert '[ASYNC_EXEC_RESET_OK]' in text # 不以旧映像输出冒充exec。
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

**更新正常输入等待。** 先等完整P5，仍走真实UART和PS/2。

<!-- write: scripts/console_test.py -->
```python
"""Exercise the running guest through QEMU keys and real UART receive bytes."""  # 两种设备输入均进入真正运行的内核控制台
import json    # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import socket  # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from boot import command, qmp_execute, qmp_screenshot  # 复用启动、QMP 请求与截图接口，输入检查继续等待 guest 完成记录。
from tools import ROOT  # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。

def check_console(profile='normal', uart_input=None):  # 每次启动独立guest，按uart_input选择UART字节或PS/2按键。
    graphics = profile.startswith('desktop-') and not profile.endswith('no-framebuffer')  # 根据实际构建选择有无图像断言。
    if uart_input is None:  # 没有显式选择设备时，串口目标使用 UART。
        uart_input = not graphics  # 默认串口目标走UART，桌面目标走PS/2；调用者可显式改选。
    work=ROOT/'artifacts'/('console-'+profile+('-ps2' if not uart_input else '-uart'))  # 不同输入路径保存独立本轮证据。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 只读取这次guest对应的日志。
    serial.unlink(missing_ok=True)  # 删除旧完成标记，不存在时允许继续。
    endpoints=tempfile.TemporaryDirectory(prefix='airios-console-',dir='/tmp')  # 短临时目录保存本次控制与UART端点，规避socket路径长度限制。
    qmp=Path(endpoints.name)/'qmp.sock'    # QMP端点只传主机控制请求，不是内核串口。
    uart=Path(endpoints.name)/'uart.sock'  # UART端点传设备收发字节，无图形时也能交互。
    cmd=command(profile,work)+['-qmp',f'unix:{qmp},server=on,wait=off']  # 复用启动参数，再添加本轮QMP服务端点。
    wire=None  # 尚未建立UART输入连接，PS/2路径保持None。
    received=bytearray()  # 可增长数组累积本次设备字节。
    if uart_input:  # 所选输入通路为UART时才建立或读取字节连接。
        cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off'  # 找到-serial参数后替换其值，把原文件输出改成设备socket。
        cmd+=['-S']  # 先暂停CPU，UART接收连接建好后再执行，以免丢早期输出。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            if uart_input:  # 所选输入通路为UART时才建立或读取字节连接。
                deadline=time.monotonic()+10  # 等待UART端点创建最多十秒。
                while not uart.exists():      # 端点尚未建立时短暂重试，不能立刻connect。
                    if process.poll() is not None or time.monotonic()>deadline:  # CPU暂停仍要求QEMU存活并及时创建端点。
                        raise RuntimeError('UART socket unavailable')  # 端点未建立时拒绝继续连接。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                wire=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)  # 本机流式UART端点。
                wire.connect(str(uart))  # 接收连接在CPU继续之前建好。
                wire.setblocking(False)  # 暂无字节时立即抛BlockingIOError。
                qmp_execute(qmp,'cont')  # 接收端准备好后通过控制连接让虚拟CPU继续。
            def output():  # 取得同一次guest的累计串口文本，UART字节同时保存文件。
                if wire:  # 有UART连接时从该连接读取；否则读取QEMU写出的日志文件。
                    while True:  # 尽量取出当前已有UART字节，无更多数据或连接结束时停止。
                        try:  # 单次非阻塞读取可能暂时无字节。
                            chunk=wire.recv(4096)  # 最多接收4096设备字节。
                        except BlockingIOError:  # 暂无字节不表示设备故障。
                            break  # 停止本轮读取，wait稍后会再调用output。
                        if not chunk:  # 空bytes说明设备连接结束。
                            break  # 结束这一轮字节收集。
                        received.extend(chunk)  # 可变bytearray追加本次真实接收字节。
                    serial.write_bytes(received)  # 把累计设备输出保存在本轮日志，失败时仍可查看。
                    return received.decode(errors='replace')  # 字节解码为文本，异常编码以替代字符呈现而不丢整份记录。
                return serial.read_text(errors='replace') if serial.exists() else ''  # 文件尚未创建时返回空串，等待循环继续。
            def wait(marker,after=0,timeout=10):  # after是旧日志字符偏移，只在之后寻找这次完成标记。
                deadline=time.monotonic()+timeout  # 每次等待按单调时间设置独立截止秒数。
                while time.monotonic()<deadline:   # 剩余时间内读取新的实际guest输出。
                    text=output()  # 获取累计文本，长度可能随guest运行增加。
                    for line in text[after:].splitlines(keepends=True):  # 只检查本次动作之后的行，保留结束符以识别尚未写完的记录。
                        if not line.endswith('\n'):  # 最后一行可能只有半条状态，不能据此通过。
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]', '[AIRI_P4_READY]', '[AIRI_P5_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
                    if process.poll() is not None:  # 运行进程提前退出，即使旧日志有文字也不能当成本次输入成功。
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P5_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
            def update(view,commands,page=0):  # 组装与内核输出字段顺序一致的具体完成标记。
                return f'[CONSOLE_UPDATE] view={view} commands={commands} page={page}'  # f字符串把预期页面、累计命令数与内存页号代入文本。
            def capture(path):  # 只有实际图形目标才读取显示设备。
                if graphics:  # 串口或缺帧缓冲目标不产生截图。
                    qmp_screenshot(qmp, path)  # 在命令完成后保存本轮画面。
            commands=0  # 本次guest已提交命令数，嵌套run每次递增。
            def run(text,expect):  # 把整条终端命令送虚拟键盘，等待这次回车对应记录并核对输出。
                nonlocal commands     # 修改check_console作用域变量，不在run内另建未初始化的局部计数。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in text:  # 字符串逐个字符迭代。
                    key('spc' if c==' ' else 'minus' if c=='-' else c)  # 空格/横线用QEMU键名，其余直接送字符名。
                key('ret')   # 回车使guest处理当前完整命令。
                commands+=1  # 预期累计提交数增加一。
                tail=wait(update(0,commands),offset)  # 终端view=0且命令数匹配后才得到本次输出尾部。
                assert expect in tail, f'Missing output: {expect}'  # 缺少当前命令预期文字时用assert明确中止。
            if wire:  # 有UART连接时从该连接读取；否则读取QEMU写出的日志文件。
                offset=len(output())  # 记录第一条UART命令之前的日志末尾。
                wire.sendall(b'echo uart-receive-ok\r')  # bytes中的回车提交这条实际设备命令。
                tail=wait(update(0,1),offset)  # 第一条UART命令完成后取得新增输出。
                assert 'uart-receive-ok' in tail and 'graphics=0' in output()  # 无图形启动仍须实际处理UART输入与echo。
                offset=len(output())  # 第二条命令不复用第一条完成标记。
                wire.sendall(b'devices\n')     # 实际UART字节末尾换行提交。
                tail=wait(update(0,2),offset)  # 等第二条UART命令的完成记录。
                if profile.endswith('no-framebuffer'):  # 缺设备与未启用绘图是两个独立事实。
                    assert 'fb0     display   unavailable' in tail  # 设备查询明确图形不可用。
                offset=len(output())  # 新命令只观察之后的完成记录。
                wire.sendall(b'theme day\n')  # 串口目标没有主题扩展，应明确拒绝。
                wait(update(0,3),offset)      # 等待第三次真实提交。
                assert 'unknown command' in output()[offset:]  # 不把无消费者的主题设置伪装成功。
                offset=len(output()) # 内存查询使用本次命令之后的新记录。
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: hello processes worker broken'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
                    tail=wait(update(0,number),offset) # 程序会话全部收拢后才完成控制台命令。
                    assert expected in tail # 核对当前请求结果，不能复用旧标记。
                    assert '[AIRI_PANIC]' not in tail # 应用格式或退出错误不能停掉内核。
                cases=['UART receive / command dispatch' ,'serial-only device report']  # 结果只记录此路径实际运行的两项观察。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
                run('help','mem map    Complete physical address inventory')  # 验证帮助输出包含完整物理地图命令说明。
                run('mem','page pool: total=')     # 验证实际内存描述，不假装已有分配器占用统计。
                run('unknown','unknown command. Type help.')  # 未知命令必须得到具体错误文本。
                run('echo ready','ready')  # 检查文字参数到输出的正常命令通路。
                for code,view,name in [('f2',1,'memory'),('f3',2,'devices'),('f4',3,'journal'),('f1',0,'terminal-active')]:  # 每个三元组同时指定按键、预期视图和截图名称，循环解包三项。
                    offset=len(output())  # 记录切页前日志尾。
                    key(code)  # 按本三元组指定的功能键。
                    wait(update(view,commands),offset)  # 等对应视图新完成记录，命令数不变。
                    capture(work/(name+'.ppm'))  # 对应视图完成后才保存画面。
                    if code=='f2':  # 只有内存页面具有物理区域分页操作。
                        offset=len(output())  # 记录翻页之前的日志尾。
                        key('pgdn')  # 请求向后翻内存地图页。
                        wait(update(1,commands,1),offset)  # 等内存view=1/page=1完成。
                        capture(work/'memory-page2.ppm')   # 保存第二页画面，与刚等待的page=1对应。
                        offset=len(output())  # 此次向前翻页排除刚才page=1记录。
                        key('pgup')  # 请求返回首个内存页。
                        wait(update(1,commands,0),offset)  # 等page=0新完成。
                if graphics:  # 日夜命令属于图形扩展。
                    for mode in ('day', 'night'):  # 两个真实命令都走 PS/2 输入路径。
                        run('theme ' + mode, 'Appearance: ' + mode.title())  # 等待命令完成标记，不以进程仍在运行代替成功。
                        capture( work / ('theme-' + mode + '.ppm'))  # 保存真实 guest 帧缓冲，供与主机样板比较。
                run('mem map','PHYSICAL END')  # 完整地图命令输出应包含区域结束地址列。
                offset=len(output())  # 清屏前排除已有完成记录。
                key('ctrl','l')  # 同时发送Ctrl-L功能组合。
                wait(update(0,commands),offset)  # 等清屏完成，累计命令数保持。
                capture(work/'cleared.ppm')      # 清屏完成后再截帧。
                offset=len(output())  # 记录历史操作开始位置。
                key('up')    # 取回最近的mem map历史文本。
                key('ret')   # 回车形成新的命令提交。
                commands+=1  # 等待新的累计命令数。
                wait(update(0,commands),offset)  # 等待新增命令计数的完成标记。
                assert 'airi> mem map' in output()[offset:]  # 历史回取必须实际提交前一条地图命令。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'syx':  # 先输入错误命令草稿。
                    key(c)  # 将当前字符送到虚拟键盘。
                key('backspace')  # 删除草稿末尾x。
                key('s')     # 补成sys。
                key('ret')   # 提交修复后的命令。
                commands+=1  # 本次实际提交也计入预期数。
                assert 'learning kernel' in wait(update(0,commands),offset)  # 修复后的sys命令应报告当前内核身份。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'echo ':     # 逐个字符准备echo前缀。
                    key('spc' if c==' ' else c)  # 空格转spc键名。
                key('shift','a')  # 同时按Shift和a得到大写A。
                key('9')  # 普通数字9。
                key('shift','1')  # Shift-1转换为标点!。
                key('ret')   # 提交echo A9!。
                commands+=1  # 与guest累计提交计数同步。
                assert 'A9!' in wait(update(0,commands),offset)  # 等待本次完成后核对大写与标点扫描码转换。
                capture(work/'terminal-session.ppm')  # 保存全部命令序列结束时的实际终端画面。
                cases=['PS/2 command dispatch','help/mem/map/echo/errors','F1-F4 guest views','memory paging',  # 记录本次PS/2通路已执行的命令、切页和分页。
                       'Ctrl-L clear','history recall','backspace','shift letters/punctuation']  # 继续记录清屏、历史、退格和Shift观察。
            result={'profile':profile,'passed':True,'cases':cases,'seconds':round(time.monotonic()-started,2)}  # 各观察成立后才形成结果，耗时来自本次单调时钟。
            (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
            print(f'PASS console/{profile} ({result["seconds"]}s)')  # 按实际路径与耗时报告本轮输入通过。
            return result  # 当前输入路径全部观察成功，返回实际case记录。
        finally:  # 成功返回与异常离开都进入收尾。
            if wire:  # 只有UART通路建立了设备socket。
                wire.close()  # 正常和失败都关闭本轮UART连接。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是请求结束，仍需收回进程。
                process.wait(timeout=5)  # 最多五秒等待正常退出。
            except subprocess.TimeoutExpired:  # QEMU没有在截止时间前退出。
                process.kill()  # 强制结束仍存活的本轮进程。
                process.wait()  # 收回已结束进程，不留下僵尸状态。
            qmp.unlink(missing_ok=True)   # 清理本轮QMP端点。
            uart.unlink(missing_ok=True)  # 清理UART端点，PS/2通路未创建也允许。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

```sh
./airios test # 八组模型/算法、九种启动、三条输入、两次恢复启动和一次真实节流会话。
./airios run # 输入run async，再查看IO_TRACE、WAIT与最终资源回收。
```

2026-10-16本轮完整回归通过，第三会话实际接受1 IOPS并观察到另一用户在CQ登记与唤醒之间进展。正常ASYNC_USER_DEMO_OK的blocked/woken/switches来自本次运行；快路径可变化，pools=reclaimed在launch真正drain和quiescent之后才打印。只读与隔离配置分别出现ASYNC_READONLY_OK、ASYNC_UNAVAILABLE_OK。

这条路径已经是用户库→系统调用→SQ→等待与切换→真实设备→CQ→同一次用户调用继续。它仍使用固定缓冲副本、四项软件队列和单硬件在途，不提供完整Linux ABI、并行性能承诺或TCP/IP。接着通过Linux线程、epoll与liburing实验对照更完整的接口，最后用用户shell把已有程序串成可交互演示。

QMP参数依据为[QEMU官方块限速接口](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-block_set_io_throttle)，实际接受结果和限制以本轮证据为准。
