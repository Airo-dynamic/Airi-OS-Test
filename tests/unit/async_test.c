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
