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
