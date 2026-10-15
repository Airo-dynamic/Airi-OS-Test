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
