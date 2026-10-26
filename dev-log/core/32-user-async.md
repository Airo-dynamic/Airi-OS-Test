# K32 · 用户请求离开调用栈之后，谁把完成交回来

K31 的 cookie30 已走完软件 SQ、设备与 CQ，但提交者还是可信内核代码。现在让 ring3 程序做同样的写入：系统调用要复制描述并检查资源归属，CQ 暂空时还要保留那一次 next 调用，让另一个用户取得 CPU。随后在请求已开始或完成时退出、exec，确认旧资源不会交给新轮次。最后以QEMU真实限速证明一次CQ阻塞期间另一个任务取得CPU，而不要求快设备也人为等待。

<a id="async-user-abi"></a>
## 用户有两类对象，寿命不同

同一个 submit 调用里出现两份“缓冲”，它们的寿命不同。局部提交描述说明做什么，注册 buffer 保存被操作的数据。用户提交结构只有 sector 和 cookie，内核在int80返回前复制完，所以用户函数submit里的局部结构可立即结束寿命，下次调用可以复用同一地址。注册buffer却从接受请求开始保持BUSY，直到对应CQ被收集；用户put/get/free都会在BUSY时返回−16。完成前可复用提交描述，不可复用请求借用的数据资源，这两个规则不能混在一起。

例如用户把提交描述放在栈上，int80 返回后可以重用这段栈；cookie30 借用的注册 buffer 却仍为 BUSY，直到 CQ 收取。airi_io_alloc 返回正句柄，不是 C 指针。当前注册的是内核固定512字节池，put/get显式复制；设备DMA仍用驱动静态区。它不是Linux io_uring注册用户页的零拷贝接口，也不宣称兼容IORING_REGISTER_BUFFERS。范围仍限制在256–271独立实验扇区，正式日志不会被这个用户接口覆盖。

编号23申请，24 put，25 get，26 free；27提交时传操作、handle和16字节描述地址；28 next接受CQ输出与wait标志，0不等、1阻塞；29提供只读状态。next返回0表示复制出一条完成，真正I/O结果在completion.result。负CQ结果、空队列−11、坏输出−14与提交未接受是不同层次的结果。

新状态记录为40字节：queued、active、completed、registered_buffers、busy_buffers、outstanding、retired_active、reserved八个32位数，再接64位epoch。retired_active只表示同一PID旧epoch尚由内核收集的ACTIVE数量，不授权读取旧数据或CQ。所有结构都没有内核指针，可供以后串口、图形或WebUI适配消费，但还不是网络协议。

**更新公开数值记录。** 用户与内核都检查固定长度，当前程序仍以C11编译。

在已有固定记录后加入40字节状态和长度断言；原提交与完成布局保持。

<!-- replace: include/airi/io_abi.h -->
<!-- before -->
```c
    int64_t result; // 读写成功512，FLUSH成功0，错误负数。
    uint32_t operation, reserved; // 操作类型与必须为零的保留字段。
};
_Static_assert(sizeof(struct airi_io_submission) == 16, "submission ABI size"); // 读用户结构前固定字节数。
_Static_assert(sizeof(struct airi_io_completion) == 32, "completion ABI size"); // CQ输出不能随编译器结构变化。
#endif // 结束保护。
```
<!-- after -->
```c
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

要证明父等待期间孩子确实运行，需要把日志放在实际状态变化处，而不能由用户打印“已并发”就算通过。引擎新增可选 event 回调。接受SQ后报告submit，后端接收后报告start，CQ形成后报告complete；旧epoch或无主完成报告drop，关闭时尚未开始的请求报告cancel。观察器只在当前调用内借请求元数据，不能保存指针或重入修改引擎。它不影响CQ信用与设备所有权，不把数据内容或内核地址写进串口。

同样，页池计数恢复只说明动态页面归还；K31 的固定池即使泄漏，也不会改变页池统计。内部 state 按 owner 与 epoch 统计当前请求与 buffer，另外数同PID的旧ACTIVE；quiescent核对全部owner、注册buffer和请求单元。静态池不会改变物理页free数，因此仅比较页数不足以发现注册资源泄漏。新主机断言在reset后确认当前active=0、retired_active=1、outstanding=0，后端收集后retired_active归零。

**更新算法与内核服务声明。** 内部观察接口和公开io_abi分开，不向用户交池布局。

加入同步观察器事件种类与后端回调，原引擎字段布局相应接入。

<!-- replace: include/airi/async.h -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

声明只读状态、quiescent、内核 owner/缓冲操作及服务/drain 接口。

<!-- replace: include/airi/async.h -->
<!-- before -->
```c
int airi_aio_pop(struct airi_aio_engine *engine, unsigned owner, struct airi_io_completion *completion); // 空返回-11，收集后才解锁缓冲。
int airi_aio_pump(struct airi_aio_engine *engine); // 有界一次服务，不睡眠；内部不变量错误-5。
bool airi_aio_work(const struct airi_aio_engine *engine); // 包含无owner的ACTIVE，不含仅等待用户收集的CQ。
void airi_async_demo(void); // 固定QEMU内核队列演示，用户API接下一检查点。
#endif // 结束保护。
```
<!-- after -->
```c
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

**更新引擎的六组窗口。** K31接受/收集顺序保留，只加入同步只读观察、状态与完全回收检查。

在查找和初始化处接入可选观察器。回调只借当前元数据，不改变引擎所有权。

<!-- replace: kernel/io/engine.c -->
<!-- before -->
```c
        return NULL; // 无效句柄。
    struct airi_aio_buffer *buffer = &e->buffers[index]; // 索引已限制。
    return buffer->state != AIRI_AIO_BUFFER_FREE && buffer->owner == owner->id && buffer->epoch == owner->epoch && buffer->handle == handle ? buffer : NULL; // 旧句柄和其他owner都不能命中。
}
int airi_aio_init(struct airi_aio_engine *e, const struct airi_aio_backend *backend, uint64_t base, uint64_t sectors) { // 仅用于无在途借用的新对象。
    if (!e || !backend || !backend->begin || !backend->poll || !sectors || base > UINT64_MAX - sectors) // 检查后端与有界扇区范围。
```
<!-- after -->
```c
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
```

关闭时报告 QUEUED 的 cancel，ACTIVE 继续等真实后端完成。

<!-- replace: kernel/io/engine.c -->
<!-- before -->
```c
    if (!owner) // 无资源可撤销。
        return; // 不影响其他owner。
    for (unsigned i = 0; i < AIRI_AIO_REQUESTS; ++i) // 只取消尚未交给设备的排队项。
        if (e->requests[i].state == AIRI_AIO_QUEUED && e->requests[i].owner == id && e->requests[i].epoch == owner->epoch) // ACTIVE仍需被真实poll收集。
            airi_memset(&e->requests[i], 0, sizeof(e->requests[i])); // 未提交请求没有后端借用，可立即复用。
    for (unsigned i = 0; i < AIRI_AIO_BUFFERS; ++i) // 注册缓冲不被DMA直接使用。
        if (e->buffers[i].owner == id && e->buffers[i].epoch == owner->epoch) // 只撤销本轮次所有权。
            airi_memset(&e->buffers[i], 0, sizeof(e->buffers[i])); // 在途数据副本仍在ACTIVE单元中。
```
<!-- after -->
```c
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
```

submit 的状态全部发布以后才报告接受事件。

<!-- replace: kernel/io/engine.c -->
<!-- before -->
```c
        buffer->state = AIRI_AIO_BUFFER_BUSY; // 用户不可修改或释放，直到CQ收集。
    }
    ++owner->outstanding; // 接受时预留CQ容量，完成时不释放信用。
    return 0; // 请求可稍后开始，也可通过CQ报I/O错误。
}
static int complete(struct airi_aio_engine *e, struct airi_aio_request *request, int result) { // 后端已结束借用，才能回收请求单元。
```
<!-- after -->
```c
        buffer->state = AIRI_AIO_BUFFER_BUSY; // 用户不可修改或释放，直到CQ收集。
    }
    ++owner->outstanding; // 接受时预留CQ容量，完成时不释放信用。
    event(e, AIRI_AIO_SUBMIT, request, 0); // 已接受不表示I/O完成。
    return 0; // 请求可稍后开始，也可通过CQ报I/O错误。
}
static int complete(struct airi_aio_engine *e, struct airi_aio_request *request, int result) { // 后端已结束借用，才能回收请求单元。
```

CQ 形成后报告 complete，旧轮次或无主结果报告 drop，再回收请求单元。

<!-- replace: kernel/io/engine.c -->
<!-- before -->
```c
        unsigned tail = (owner->head + owner->count) % AIRI_AIO_CQ; // CQ环当前尾槽。
        owner->completions[tail] = (struct airi_io_completion){request->cookie, request->handle, result ? result : request->operation == AIRI_IO_FLUSH ? 0 : 512, request->operation, 0}; // cookie关联身份，不推断完成顺序。
        ++owner->count; // 结果完全写好才发布可收集数量。
    }
    airi_memset(request, 0, sizeof(*request)); // 无主完成丢弃，ACTIVE副本也只在真实收集后才复用。
    return 0; // 完成信用仍留到pop或owner关闭。
}
```
<!-- after -->
```c
        unsigned tail = (owner->head + owner->count) % AIRI_AIO_CQ; // CQ环当前尾槽。
        owner->completions[tail] = (struct airi_io_completion){request->cookie, request->handle, result ? result : request->operation == AIRI_IO_FLUSH ? 0 : 512, request->operation, 0}; // cookie关联身份，不推断完成顺序。
        ++owner->count; // 结果完全写好才发布可收集数量。
        event(e, AIRI_AIO_COMPLETE, request, owner->completions[tail].result); // 真实CQ已形成，记录对应结果。
    } else // 原owner退出或已经进入新epoch。
        event(e, AIRI_AIO_DROP, request, result ? result : request->operation == AIRI_IO_FLUSH ? 0 : 512); // 后端已结束，完成不会交给新对象。
    airi_memset(request, 0, sizeof(*request)); // 无主完成丢弃，ACTIVE副本也只在真实收集后才复用。
    return 0; // 完成信用仍留到pop或owner关闭。
}
```

后端接受且 ACTIVE 状态完整以后报告 start。

<!-- replace: kernel/io/engine.c -->
<!-- before -->
```c
        if (!result) { // 只有被后端接收才变ACTIVE。
            next->state = AIRI_AIO_ACTIVE; // 数据副本保持到真实poll。
            e->active = (int)(next - e->requests); // 数组内指针差得到本槽。
            return 0; // 不在同次pump里等待它完成。
        }
        if (result > 0) // begin只允许0或负错误。
```
<!-- after -->
```c
        if (!result) { // 只有被后端接收才变ACTIVE。
            next->state = AIRI_AIO_ACTIVE; // 数据副本保持到真实poll。
            e->active = (int)(next - e->requests); // 数组内指针差得到本槽。
            event(e, AIRI_AIO_START, next, 0); // 所有在途状态完整发布后才记录开始。
            return 0; // 不在同次pump里等待它完成。
        }
        if (result > 0) // begin只允许0或负错误。
```

在末尾加入状态快照与 quiescent：分别统计当前轮次、退役 ACTIVE 和全部资源池。

<!-- replace: kernel/io/engine.c -->
<!-- before -->
```c
            return true; // 还不能丢弃引擎对象。
    return false; // 只有用户尚未收集的CQ不要求设备推进。
}
```
<!-- after -->
```c
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

用户提交返回后，设备还需要有人持续 pump。若只在提交者再进系统调用时推进，它正等 CQ 就可能永远没有完成；若在 IRQ 里直接复制用户数据，又会把长操作塞进中断链。这里把时机与实际工作分开。submit 只入 SQ。普通系统调用尾部poll_waiters可以取已经形成的CQ，但不直接pump设备。时钟调度发现IO_WAIT或任何异步工作时暂回启动栈；launch的服务循环先pump一次，再重查等待者，然后公平选择用户。没有用户可运行则真正等PIT。即使另一个用户纯计算不调用系统调用，设备仍能获得周期服务。

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

接入架构等待接口。

<!-- replace: kernel/io/service.c -->
<!-- before -->
```c
#include <airi/async.h> // 有限SQ/CQ与注册缓冲所有权。
#include <airi/block.h> // K30可信单消费者的begin/poll。
#include <airi/kernel.h> // 真设备演示与内部错误报告。
```
<!-- after -->
```c
#include <airi/arch.h> // 空闲等待由当前CPU适配提供。
#include <airi/async.h> // 有限SQ/CQ与注册缓冲所有权。
#include <airi/block.h> // K30可信单消费者的begin/poll。
#include <airi/kernel.h> // 真设备演示与内部错误报告。
```

增加真实观察回调、静态池的状态与生命周期适配；最后 drain 无主在途并核对 quiescent。

<!-- replace: kernel/io/service.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

启动演示的等待改用架构 idle，pump 本身仍不休眠。

<!-- replace: kernel/io/service.c -->
<!-- before -->
```c
        if (airi_aio_pump(&engine)) // 每次有界检查，不在算法内睡眠。
            airi_panic("async engine invariant failed"); // 内部错误不能当普通设备结果。
        if (airi_aio_work(&engine)) // 下一次服务需要等待设备或PIT进展。
            __asm__ volatile("sti; hlt" ::: "memory"); // 这里只是启动演示的等待策略，不属于submit/pump接口。
    }
    if (airi_aio_submit(&engine, 7001, AIRI_IO_FLUSH, 0, 0, 50) != -11 || airi_aio_buffer_free(&engine, 7001, (uint64_t)writer) != -16) // SQ已空但四个CQ信用仍占用，buffer仍BUSY。
        airi_panic("uncollected CQ released resources too early"); // 硬件完成不等于调用者已收集。
```
<!-- after -->
```c
        if (airi_aio_pump(&engine)) // 每次有界检查，不在算法内睡眠。
            airi_panic("async engine invariant failed"); // 内部错误不能当普通设备结果。
        if (airi_aio_work(&engine)) // 下一次服务需要等待设备或PIT进展。
            airi_arch_idle(); // 这里只是启动演示的等待策略，不属于submit/pump接口。
    }
    if (airi_aio_submit(&engine, 7001, AIRI_IO_FLUSH, 0, 0, 50) != -11 || airi_aio_buffer_free(&engine, 7001, (uint64_t)writer) != -16) // SQ已空但四个CQ信用仍占用，buffer仍BUSY。
        airi_panic("uncollected CQ released resources too early"); // 硬件完成不等于调用者已收集。
```

<a id="async-process-owner"></a>
## fork复制整数，exec与exit处理资源所有权

fork 先复制用户页，所以孩子局部变量里也会出现父 handle 的整数；但资源授权并没有跟随整数复制。新进程完成映像和 PID 准备后才 open owner，fork 给孩子空上下文。内核以孩子 PID 查父 handle，仍返回 −9。这是本接口的策略，不能套用 Linux 共享 ring/FD 规则。

exec 保持 PID，却需要让新映像与旧 I/O 隔离。因此先完整准备新 ELF，再 reset 当前 PID 的 owner。reset 在撤销前确认新 epoch 可产生；失败就清 next，旧映像和 I/O 资源继续有效。成功后撤销旧 QUEUED 与 CQ，ACTIVE 的稳定副本仍留在内核等待后端结束。

提交新所有者时，把 old 临时控制块的 async_owned 清 false，只让 next 持新轮次；否则清旧页表会连带关闭新 owner。PID 同样在回绕前拒绝，避免重新出现 0 或旧身份。数字身份和轮次共同承担的是资源联系，不能靠槽位置或 C 指针代替。

exit 立即 close owner、清 async_owned、关闭 FD，再进入僵尸。ACTIVE 此后既不借用户页，也不借已销毁 owner 指针，仍可能在设备上继续。全部用户结束后先回收页与栈，再由长期内核对象 drain 无主 ACTIVE，最后检查静态池完全回收。已开始写的副作用可能在退出以后发生，退出不等于撤销设备写；本节生命周期演示使用读请求，不改正式日志。

CQ 交回用户也有一个不可提前消费的边界。next 先验证完整 32 字节输出，再尝试 pop；空且 wait=1 才保存用户地址与原 frame，设 IO_ASYNC。普通服务随后经目标页表复制完成，写 frame.RAX=0，再设 RUNNABLE。非法或只读输出必须在 pop 前拒绝，否则用户没得到结果，CQ、信用与 BUSY 却已经被消费。

**依次更新进程路径的旧/新窗口。** 接口、owner标志、fork/exec/exit、CQ等待和服务循环要一起更新；相同旧代码保留。

先接入异步服务和架构接口声明。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
```
<!-- after -->
```c
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
```

增加 IO_ASYNC 及 async_owned。标志说明谁负责关闭当前 owner，和用户页中的 handle 整数分开。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
```
<!-- after -->
```c
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
```

登记同一 async ELF 的边界。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
```

为同一 ELF 注册 async 与 async-reset，两次入口由 argv 选择。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"waits", airi_waits_elf_start, airi_waits_elf_end}, // 阻塞流和就绪条件等待。
    {"pipes", airi_pipes_elf_start, airi_pipes_elf_end}, // 有界FIFO与关闭语义。
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
```
<!-- after -->
```c
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"async", airi_async_elf_start, airi_async_elf_end}, // 完整用户提交/完成路径。
    {"async-reset", airi_async_elf_start, airi_async_elf_end}, // 同一PID新epoch的验证入口。
    {"waits", airi_waits_elf_start, airi_waits_elf_end}, // 阻塞流和就绪条件等待。
    {"pipes", airi_pipes_elf_start, airi_pipes_elf_end}, // 有界FIFO与关闭语义。
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
```

等待者扫描增加 CQ 分支：先 pop 到内核记录，经目标页表交回用户，再发布原 frame 的结果与运行资格。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (p->state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (p->io == IO_READY) { // 订阅的是条件，不是一次成功read的承诺。
            result = airi_fd_ready(&p->files, p->io_fd); // 当前条件可能被其他进程消费或改变。
            if (result >= 0) // 负错误必须原样唤醒，不能按位筛掉。
                result &= p->io_events | AIRI_READY_HUP | AIRI_READY_ERROR; // 对端关闭即使未显式请求也应可见。
```
<!-- after -->
```c
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
```

cleanup 只关闭确实拥有的 owner，再清已有进程资源；失败构造也走同一归还路径。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    airi_fd_close_all(&p->files, false); // 回滚或已退出空表均安全，不能遗留全局引用。
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
```
<!-- after -->
```c
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
```

时钟调度在存在异步工作时转普通服务栈，避免纯计算用户使设备永远得不到 pump。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        old->state = USER_RUNNABLE; // 时间片结束仍可运行。
    if (reason != REASON_TIMER) // 普通内核边界可重试等待条件。
        poll_waiters(); // 已有字节、关闭或错误形成后再唤醒。
    else // IRQ只检查状态，并把可延后服务交给启动栈。
        for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 即使另有纯计算用户，也不能饿死超时与未来设备完成。
            if (processes[i].state == USER_IO_WAIT) // 有等待源需要周期服务。
                return pause_for_service(true); // 不在IRQ C链里复制用户数据或输出。
    for (unsigned n = 1; n <= PROCESS_COUNT; ++n) { // 从old之后轮转一圈。
        struct user_process *next = &processes[(slot(old) + n) % PROCESS_COUNT]; // PID与槽仍分开。
        if (next->state == USER_RUNNABLE) { // 仅可运行者有可返回的用户现场。
```
<!-- after -->
```c
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
```

exit 先关闭本轮 owner 并清持有标志；ACTIVE 的内核副本继续由服务对象持有。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前进程内核栈上，先结束身份而不释放栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
```
<!-- after -->
```c
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
```

在 fork 分配新 PID 前拒绝编号回绕。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
```
<!-- after -->
```c
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    if (next_pid == UINT32_MAX) // 固定32位正PID不允许回绕命中旧身份。
        return -75; // -EOVERFLOW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
```

孩子页复制成功后建立空 owner；父 handle 的整数副本没有授权，失败统一回滚孩子。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    child->heap_next = parent->heap_next; // 复制后的用户页分配游标也属于子空间。
    place_frame(child); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->count); // 实际复制数量含用户栈与堆。
```
<!-- after -->
```c
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    child->heap_next = parent->heap_next; // 复制后的用户页分配游标也属于子空间。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
    place_frame(child); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->count); // 实际复制数量含用户栈与堆。
```

exec 在候选 ELF 齐全以后 reset，同步撤销前先核对 epoch；提交时只让 next 持有新轮次，清旧控制块不会关闭它。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    struct user_process old = *current; // 暂存旧用户资源，稍后在切离旧根之后归还。
    unsigned old_pages = old.count; // 归还前保留实际替换规模用于记录。
    next.pid = old.pid; // exec保持进程身份。
    next.parent = old.parent; // 父仍等待同一个孩子。
```
<!-- after -->
```c
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
```

在原调用之后加入 23–29，分别核对句柄、用户副本、描述与 CQ 输出；坏输出在 pop 以前拒绝。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        }
        f->rax = (uint64_t)result; // 立即就绪、纯查询、错误统一返回。
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
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
```

根映像启动也先拒绝 PID 回绕，尚未取得资源时就失败。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static int launch(const uint8_t *image, size_t bytes, const char *name, bool elf, struct user_result *result) { // 可用于启动演示或串口命令，程序失败不等于内核失败。
    if (session_active) // 当前只有一个由启动栈收拢的用户会话。
        return -16; // -EBUSY，不能覆盖已有kernel_return。
    struct airi_memory_stats before, after; // 包含根、所有孩子和内核栈。
    airi_memory_snapshot(&before); // 在任何资源分配前取基准。
    struct user_process *root = unused(); // 上次会话已回收的空槽。
```
<!-- after -->
```c
static int launch(const uint8_t *image, size_t bytes, const char *name, bool elf, struct user_result *result) { // 可用于启动演示或串口命令，程序失败不等于内核失败。
    if (session_active) // 当前只有一个由启动栈收拢的用户会话。
        return -16; // -EBUSY，不能覆盖已有kernel_return。
    if (next_pid == UINT32_MAX) // 不能把回绕后的0当合法新owner。
        return -75; // -EOVERFLOW，尚未分配任何资源。
    struct airi_memory_stats before, after; // 包含根、所有孩子和内核栈。
    airi_memory_snapshot(&before); // 在任何资源分配前取基准。
    struct user_process *root = unused(); // 上次会话已回收的空槽。
```

根映像完整准备后才 open owner，成功后再发布给用户。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    place_frame(root); // 寄存器值进入本进程真实内核栈。
    root->state = USER_RUNNABLE; // 发布首次运行资格。
    root_pid = root->pid; // 会话根身份不会被孩子exit覆盖。
```
<!-- after -->
```c
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
```

launch 每次从架构 gate 返回，先辨认会话是否结束；暂时服务时 pump 再重查等待者。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    session_idle = false; // 首次int129创建新活动会话。
    long code; // 根结束结果由启动现场RAX交回。
    for (;;) { // 每次返回启动栈，先区分暂时服务和整个会话结束。
        __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // 用户运行期间启动栈与继续现场保持完整。
        if (!session_active) // 只有所有用户结束才离开launch。
            break; // code此时才是根用户真实退出值。
        collect_pending(); // 当前确在启动栈，仍保守跳过current槽。
        for (;;) { // 暂无可运行者时在长期内核根上服务等待条件。
            poll_waiters(); // 可复制用户结果和记录日志，不在IRQ C链里做。
            struct user_process *next = NULL; // 每次重查后重新选择。
            for (unsigned n = 1; n <= PROCESS_COUNT; ++n) // 轮转保持公平，不总挑槽0。
```
<!-- after -->
```c
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
```

没有可运行用户时调用架构 idle；醒来仍回同一普通服务栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
                break; // 回外层重新登记新的启动继续现场。
            }
            ++idle_halt_count; // 统计真正没有可运行用户后执行的空闲等待。
            __asm__ volatile("sti; hlt" ::: "memory"); // CPU真正空闲等PIT，醒后仍在启动服务栈。
        }
    }
    *result = (struct user_result){(int)code, root->user_ticks, timer_switches, wait_blocks, root->fault_error, io_block_count, io_wake_count, idle_halt_count}; // 回收前复制必要数值。
```
<!-- after -->
```c
                break; // 回外层重新登记新的启动继续现场。
            }
            ++idle_halt_count; // 统计真正没有可运行用户后执行的空闲等待。
            airi_arch_idle(); // CPU真正空闲等PIT，醒后仍在启动服务栈。
        }
    }
    *result = (struct user_result){(int)code, root->user_ticks, timer_switches, wait_blocks, root->fault_error, io_block_count, io_wake_count, idle_halt_count}; // 回收前复制必要数值。
```

全部用户页和栈收拢以后，仍需 drain 无主 ACTIVE，再核对固定池；动态页空闲数不能证明静态池已回收。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    kernel_return = NULL; // 下次会话重新登记。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
```
<!-- after -->
```c
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
```

最后运行 async 工作负载，并以真实阻塞/唤醒计数报告结果；快设备可以直接取得 CQ。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (launch(airi_waits_elf_start, (size_t)((uintptr_t)airi_waits_elf_end - (uintptr_t)airi_waits_elf_start), "waits", true, &waits_result) || waits_result.code || waits_result.io_blocks < 4 || waits_result.io_wakes < 4 || !waits_result.idle_halts) // 读、写、空闲与纯计算并行超时四种实际等待均须发生。
        airi_panic("blocking I/O did not preserve waiting semantics"); // 不把用户循环重试当内核阻塞。
    airi_log("[WAIT_DEMO_OK] blocked=%u woken=%u idle_halts=%u", waits_result.io_blocks, waits_result.io_wakes, waits_result.idle_halts); // 统计来自真实状态变化。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files storage recover pipes waits"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```
<!-- after -->
```c
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
```

<a id="async-user-program"></a>
## 正常、受限设备与新映像分别验证

async先申请四个注册对象并验证第五个−28，再释放spare验证旧handle失效；坏提交指针−14不锁reader。fork孩子用父handle读得到−9，再在约十个全局tick的窗口内反复取得CPU工作，中途打印实际进展；这不是保证十tick都独占CPU。父提交写cookie30、FLUSH cookie10、读cookie20和越界cookie40，未收集时第五个提交因完成信用返回−11，buffer仍BUSY。

父每次next可在空CQ真正阻塞，也可立即取得快设备已完成的记录。按cookie而非位置核对，正常设备读回512字节模式；只读写CQ为−30，读和FLUSH仍须正常；已隔离设备有效I/O以CQ−5返回，越界仍CQ−22。受限配置明确结束错误路径，不伪装完整正常生命周期。

四条正常 CQ 证明结果能够交回，还要在提交者消失时检查旧结果会交给谁。正常分支再启动读请求孩子并退出；父在复用槽的新buffer写parent-new，提交FLUSH并收集。单硬件消费者必须先收集旧ACTIVE才开始这个FLUSH，因此随后验证新内容不变时，旧设备借用已结束。另一个孩子发读后exec async-reset，新程序确认注册集合和CQ为空，写new-image并等retired_active归零，再确认内容未被旧完成覆盖。设备很快时关闭可能发生在完成后，轨迹会准确显示实际active或completed，不强迫固定时序。

最后根进程提交读和FLUSH却不收集即退出；内核根据当时阶段cancel或drop，再排空并核对池。既有默认实测中观察到退出孩子和exec孩子都曾active=1，随后出现各自旧epoch的drop；这不是所有机器每次都必须重复的时间保证。

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

用户头只包含公开io_abi，不包含内核固定池结构。

<!-- replace: labs/airios/include/airi_libc.h -->
<!-- before -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
#include <stddef.h> // 用户对象字节长度。
#include <stdint.h> // 用户ABI的64位寄存器值。
void *memcpy(void *destination, const void *source, size_t bytes); // 双方不重叠的用户空间复制。
```
<!-- after -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
#include <airi/io_abi.h> // 自有固定宽度提交、完成和只读状态，不包含内核结构。
#include <stddef.h> // 用户对象字节长度。
#include <stdint.h> // 用户ABI的64位寄存器值。
void *memcpy(void *destination, const void *source, size_t bytes); // 双方不重叠的用户空间复制。
```

在已有wait_ready声明后加入23–29包装声明，错误、长度与BUSY条件分别保留。

<!-- replace: labs/airios/include/airi_libc.h -->
<!-- before -->
```c
long airi_pipe(unsigned ends[2], unsigned nonblocking); // nonblocking=0阻塞，1暂不可完成时返回-11。
long airi_ready(unsigned fd); // 管道当前条件查询，不是Linux epoll ABI。
long airi_wait_ready(unsigned fd, unsigned events, unsigned ticks); // READ/WRITE条件，0纯查询，最多500tick，超时返回0。
#endif // 结束包含保护。
```
<!-- after -->
```c
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

在末尾追加注册、复制、提交、收取与状态包装；旧系统调用的实现不用再次写入。

<!-- replace: labs/airios/libc/syscall.c -->
<!-- before -->
```c
long airi_wait_ready(unsigned fd, unsigned events, unsigned ticks) { // 条件满足返回事件位，超时0，负值是错误。
    return call3(22, fd, events, ticks); // 内核保存现场并在同一次调用后恢复。
}
```
<!-- after -->
```c
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

在末尾增加async的同一ELF边界；async-reset仍经argv选择入口，不重复包装另一份字节。

<!-- replace: labs/airios/images.S -->
<!-- before -->
```asm
airi_waits_elf_start: /* 文件开始。 */
.incbin AIRI_WAITS_ELF /* 本配置独立ELF。 */
airi_waits_elf_end: /* 半开右端。 */
```
<!-- after -->
```asm
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

用户编译搜索路径加入公开 include。

<!-- replace: Makefile -->
<!-- before -->
```make
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
```
<!-- after -->
```make
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
```

hello 对象增加 io_abi.h 依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
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
```
<!-- after -->
```make
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
```

包装对象依赖 async.elf，别名不重复链接另一份 ELF。

<!-- replace: Makefile -->
<!-- before -->
```make
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
```
<!-- after -->
```make
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
```

worker 对象也记录公开 ABI 头依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
```

files 对象记录同一头依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
```

storage 对象记录同一头依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
```

pipes 对象记录同一头依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
```

waits 对象记录同一头依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
```

新增 async 独立编译链接规则。

<!-- replace: Makefile -->
<!-- before -->
```make
$(OUT)/user/waits.elf: $(USER_COMMON) $(OUT)/user/waits.o labs/airios/program.ld
# 低地址静态链接，不借宿主入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@
```
<!-- after -->
```make
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

“另一个用户曾运行”与“它在父这一次等待期间运行”不是同一个证据。必须把进展放进同 PID 的登记和唤醒之间。快设备可能在调用 next 前已有 CQ，因此不能把“每次都应阻塞”写成验收。storage_test复用已有UART/QMP生命周期，在两个新CPU恢复同一日志之后，再启第三个guest；启动完成才用QMP block_set_io_throttle将独立airi-disk限制为1 IOPS。QEMU实际推迟设备I/O，guest没有为演示伪造延迟。

第三次run async的记录只消费该命令之后的新输出。程序第一次fork提供实际父PID，测试逐个寻找这个PID的CQ登记与对应WAKE，要求ASYNC_OTHER_PROGRESS出现在其中一个等待区间内，同时有真实start和complete轨迹。这样才证明另一个用户在父同一次CQ等待期间运行，不能仅凭QEMU仍存活或一行静态“并发”文字通过。

Python正则的r前缀保留反斜杠，\[匹配字面左方括号，(\d+)捕获一组十进制数字。例如本次命令若记录parent=29，group(1)取得字符串29；finditer逐个给出匹配区间，start/end是这次输出中的字符偏移。find从登记之后找同PID完整result=0加CRLF，缺失返回−1，无法满足中途进展位于两者之间的条件。所有范围只用本次命令之后的新文本。

限速只用于第三个新guest，前面的启动日志实验不受影响；每个实际块请求仍受首次begin的两秒截止保护。结果记录restart_boots=2、boots=3和other_user_during_cq_wait=true，原始限速参数只在QMP接受后写出。受控QEMU退出仍不冒充主机掉电，正式日志区与异步块区也始终分开。

**更新真实会话验证。** 双启动恢复保留，第三次单独证明慢设备CQ等待和其他用户进展。

引入按真实 PID 关联轨迹的正则。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
"""Run two actual QEMU boots against one dedicated, freshly created test disk.""" # 跨VM启动与模型crash分别验证。
import json # 记录本轮事实，不执行JSON内容。
from pathlib import Path # 构造项目内独立证据路径。
import socket # 实际UART收发。
```
<!-- after -->
```python
"""Run two actual QEMU boots against one dedicated, freshly created test disk.""" # 跨VM启动与模型crash分别验证。
import re # 从实际PID和完成记录关联等待区间，不固定猜PID。
import json # 记录本轮事实，不执行JSON内容。
from pathlib import Path # 构造项目内独立证据路径。
import socket # 实际UART收发。
```

撤销旧限速成功记录，新增第三会话的 QMP 限速与本轮确认；前两次恢复保持。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
    work.mkdir(parents=True, exist_ok=True) # 证据父目录必须存在。
    result_path = work / 'result.json' # 本轮状态路径。
    result_path.write_text(json.dumps({'passed':False, 'status':'running'}) + '\n') # 旧成功不能覆盖新失败。
    disk = work / 'disk.raw' # 此测试每次从空盘开始，普通run目录不受影响。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024)): # 未知对象不能当测试盘删除。
        raise RuntimeError('Invalid restart-test disk; refusing to replace') # 明确保留异常路径。
    disk.unlink(missing_ok=True) # 只删除本测试上次生成的8MiB盘，下一command排他创建新盘。
    started = time.monotonic() # 整组测试耗时。
    def boot_once(label, kernel_marker, request, user_marker): # 同一work保存盘，端点和CPU每次新建。
        with tempfile.TemporaryDirectory(prefix='airios-store-', dir='/tmp') as directory: # 短临时路径仅放通信端点。
            qmp = Path(directory) / 'qmp.sock' # QMP不是设备输出。
            uart = Path(directory) / 'uart.sock' # UART在CPU继续前连接。
```
<!-- after -->
```python
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
```

每次先等 P5，再按当前设备状态核对结果。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
                                raise RuntimeError(f'Guest exited before {marker}') # 保留目标身份。
                            time.sleep(.05) # 等设备与CPU推进。
                        raise RuntimeError(f'Restart test timed out: {marker}; see {serial}') # 明确失败文件。
                    text = wait('[AIRI_P4_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', offset) # 等命令与用户会话全部收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                finally: # 正常和错误均收回设备与CPU。
                    if wire is not None: # 建立过才关闭。
                        wire.close() # 结束本次UART连接。
```
<!-- after -->
```python
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
```

第三个新 guest 运行 async，要求同一 PID 的 CQ 等待区间内确有另一个用户进展，再报告三次会话结果。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
                        process.wait() # 不遗留僵尸。
    boot_once('first', '[STORAGE_RECOVERED] count=0 truncated=0', 'run storage', '[STORAGE_USER_OK]') # 空盘上由用户实际追加并同步。
    boot_once('second', '[STORAGE_RECOVERED] count=1 truncated=0', 'run recover', '[RECOVERY_USER_OK]') # 新CPU和新内存从同一盘恢复。
    result = {'passed':True, 'profile':profile, 'boots':2, 'recovered_records':1, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage restart: ring3 append/flush -> new QEMU -> scan/read') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
```
<!-- after -->
```python
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

入口说明同步三会话观察的职责。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 两个独立CPU从同一实验盘恢复。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
```
<!-- after -->
```python
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
```

仍调用同一测试入口，结果新增真实慢设备 CQ 等待。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 独立正式日志区的真实跨QEMU恢复。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
```
<!-- after -->
```python
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
```

**更新初始化与P5标记。** 可写正常设备完成用户生命周期后才发布AIRI_P5_READY；Linux结果另记。

所有设备配置初始化固定引擎，错误仍通过对应结果报告。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_demo(); // 三个真实设备操作和一个有界范围错误完成。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
```
<!-- after -->
```c
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_init(); // 所有设备配置都初始化固定资源池，设备错误由CQ表达。
    airi_async_demo(); // 三个真实设备操作和一个有界范围错误完成。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
```

可写正常设备完成生命周期才发布 P5；只读和隔离保留各自较早能力。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
        airi_log("[AIRI_P4_READY] files=ram pipes=blocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
```
<!-- after -->
```c
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
        airi_log("[AIRI_P4_READY] files=ram pipes=blocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
    if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
        airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
```

**更新启动预期。** 正常等P5，只读只宣称P4可读能力，隔离设备保留P3并核对错误CQ。

正常/只读/隔离分别等 P5/P4/P3，不能混用。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' # 正常配置等完整存储阶段，隔离盘只保留进程可用标记。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
```
<!-- after -->
```python
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' if case == 'block-readonly' else '[AIRI_P5_READY]' # 正常等完整异步阶段，受限设备只发布实际可用能力。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
```

核对用户异步结果与对应受限分支。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert async_marker in text # 接收、忙与完成身份须符合当前设备。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        assert '[WAIT_USER_OK]' in text and '[WAIT_IDLE_OK]' in text and '[WAIT_DEMO_OK]' in text # 真阻塞、超时与现场继续均已完成。
                        assert text.index('[WAIT_TIMEOUT_OK]') < text.index('[WAIT_BUSY_DONE]') # 纯计算用户不能饿死等待超时。
                        assert '[PIPE_USER_OK]' in text # 有界流、背压、关闭与fork共享均已实际运行。
```
<!-- after -->
```python
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
```

**更新正常输入等待。** 先等完整P5，仍走真实UART和PS/2。

READY 的完整字段匹配认识 P5。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]', '[AIRI_P4_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
```
<!-- after -->
```python
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]', '[AIRI_P4_READY]', '[AIRI_P5_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
```

输入前先等完整 P5，输入动作不变。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P4_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
```
<!-- after -->
```python
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P5_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
```


```sh
./airios test # 八组模型/算法、九种启动、三条输入、两次恢复启动和一次真实节流会话。
./airios run # 输入run async，再查看IO_TRACE、WAIT与最终资源回收。
```

2026-10-16 的既有完整回归通过，第三会话实际接受1 IOPS并观察到另一用户在CQ登记与唤醒之间进展。正常ASYNC_USER_DEMO_OK的blocked/woken/switches来自该次实际运行；快路径可变化，pools=reclaimed在launch真正drain和quiescent之后才打印。只读与隔离配置分别出现ASYNC_READONLY_OK、ASYNC_UNAVAILABLE_OK。

这条路径已经是用户库→系统调用→SQ→等待与切换→真实设备→CQ→同一次用户调用继续。它仍使用固定缓冲副本、四项软件队列和单硬件在途，不提供完整Linux ABI、并行性能承诺或TCP/IP。接着通过Linux线程、epoll与liburing实验对照更完整的接口，最后用用户shell把已有程序串成可交互演示。

QMP参数依据为[QEMU官方块限速接口](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-block_set_io_throttle)，实际接受结果和限制以该次证据为准。
