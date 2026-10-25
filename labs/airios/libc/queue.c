#include <airi_queue.h> // 四槽环、互斥和两个变化通知。
#include <airi_libc.h> // 同一次名义PIT截止，不借宿主时间。
static long prepare(airi_queue *queue, unsigned ticks, uint64_t *deadline) { // 每个公开操作只建立一个等待期限。
    if (!queue || (uintptr_t)queue % _Alignof(airi_queue) || ticks > 6000) return -22; // 拒绝显式坏形状/预算，任意坏映射仍可能用户故障。
    uint64_t now = ticks ? airi_ticks() : 0; // 零预算不需要时钟，正预算覆盖锁和条件等待的重试。
    if (now > UINT64_MAX - ticks) return -75; // 不让截止加法回绕。
    *deadline = now + ticks; // 私人栈输出只在成功后使用。
    return 0; // 此时尚未访问队列普通字段。
}
static long take(airi_queue *queue, unsigned ticks, uint64_t deadline) { // 重新取得后才检查普通谓词。
    uint64_t now = ticks ? airi_ticks() : 0; // 其他执行者运行同样会消耗原预算。
    unsigned remaining = now < deadline ? (unsigned)(deadline - now) : 0; // 到期仍允许一次立即CAS，不继续睡眠。
    long result = airi_mutex_lock(&queue->guard, remaining); // 互斥和条件等待共用剩余预算。
    return !ticks && result == -110 ? -11 : result; // 非阻塞操作暂时拿不到锁，也统一为EAGAIN。
}
static long release(airi_queue *queue, long result) { // 所有已经持锁的返回路径都经过一次释放。
    long error = airi_mutex_unlock(&queue->guard); // 不在持锁返回以后借调用者补救。
    return error ? error : result; // 正常持有者/有效对象不应失败，仍不伪造释放成功。
}
static long wait_changed(airi_queue *queue, airi_event *event, unsigned ticks, uint64_t deadline) { // 调用时持guard，返回时不持guard。
    if (!ticks) return release(queue, -11); // 谓词不成立而禁止睡眠，不改队列或输出。
    long expected = airi_event_sequence(event); // 必须在同一次谓词检查的锁内取得旧版本。
    if (expected < 0) return release(queue, expected); // 失败不能窄化成合法快照。
    long error = release(queue, 0); // 确实放开guard以后，另一方才有权改变谓词。
    if (error) return error; // 不在释放失败后继续安排睡眠。
    uint64_t now = airi_ticks(); // 释放和切换消耗的时间也计入原截止。
    if (now >= deadline) return -110; // 相减前拒绝到期。
    return airi_event_wait(event, (uint32_t)expected, (unsigned)(deadline - now)); // 0仅要求外层重新拿锁检查，不能直接操作槽。
}
long airi_queue_put(airi_queue *queue, uint64_t value, unsigned ticks) { // 64位值在成功以后归队列保存。
    uint64_t deadline; // 各次重试共享这一份截止。
    long result = prepare(queue, ticks, &deadline); // 失败前没有改变队列。
    if (result) return result; // 参数或加法错误。
    for (;;) { // 每次通知以后都从锁下的谓词重新开始。
        result = take(queue, ticks, deadline); // 不把上次等待返回当成已持有guard。
        if (result) return result; // 当前未持锁。
        if (queue->closed) return release(queue, -32); // 已关闭不接收新值，即使还有空位。
        if (queue->count < AIRI_QUEUE_CAPACITY) { // 只有锁内确认空位才能写环。
            result = airi_event_publish(&queue->available); // 先在guard内发布；版本饱和失败时尚未修改槽，醒来的人仍须取得guard。
            if (result < 0) return release(queue, result); // 版本或通知错误不把本项悄悄入队。
            queue->values[(queue->head + queue->count) % AIRI_QUEUE_CAPACITY] = value; // 首项加数量定位尾槽，环绕不移动其他项。
            ++queue->count; // 内容先完整写入，再增加可见项数，最终由guard释放发布。
            return release(queue, 0); // 已接受，不能再由生产者重复提交同一项。
        }
        result = wait_changed(queue, &queue->space, ticks, deadline); // 满时等待空位变化，释放后不能保留槽的引用。
        if (result) return result; // 到期或错误结束本次操作。
    }
}
long airi_queue_get(airi_queue *queue, uint64_t *value, unsigned ticks) { // 输出必须私人、有效且不与队列重叠。
    if (!value || (uintptr_t)value % _Alignof(uint64_t)) return -22; // 显式坏输出在取走项以前拒绝。
    uint64_t deadline; // 与put相同的总等待预算。
    long result = prepare(queue, ticks, &deadline); // 仍未修改输出。
    if (result) return result; // 队列形状/预算错误。
    for (;;) { // 醒来以后重新检查非空和关闭，而不是直接读旧槽。
        result = take(queue, ticks, deadline); // 锁仍归同一队列对象。
        if (result) return result; // 未取得，不触碰普通状态。
        if (queue->count) { // 已关闭也先排空已有项。
            result = airi_event_publish(&queue->space); // guard内先验证本次发布能前进，失败不取走值。
            if (result < 0) return release(queue, result); // 输出和环保持原状态。
            *value = queue->values[queue->head]; // 按值复制到调用者自己的输出，不借槽指针。
            queue->head = (queue->head + 1) % AIRI_QUEUE_CAPACITY; // 取走首项后环头前进一格。
            --queue->count; // 与head、输出构成同一次受保护的取出。
            return release(queue, 1); // 1明确区分取得值与结束。
        }
        if (queue->closed) return release(queue, 0); // 关闭且排空才结束，输出仍保持调用前内容。
        result = wait_changed(queue, &queue->available, ticks, deadline); // 空而开放才等待后续入队或关闭。
        if (result) return result; // 负返回不改输出。
    }
}
long airi_queue_close(airi_queue *queue, unsigned ticks) { // 关闭是广播重查，不删除已经接受的工作。
    uint64_t deadline; // 这里只可能等待guard。
    long result = prepare(queue, ticks, &deadline); // 尚未读取普通closed。
    if (result) return result; // 参数失败不关闭。
    result = take(queue, ticks, deadline); // 串行处理与其他操作的交接。
    if (result) return result; // 未持锁。
    if (queue->closed) return release(queue, 0); // 重复关闭不额外推进版本。
    result = airi_event_publish(&queue->available); // 空队列的消费者也须醒来观察结束。
    if (result >= 0) result = airi_event_publish(&queue->space); // 满队列的生产者须醒来观察拒绝后续put。
    if (result < 0) return release(queue, result); // 可能已经有一次无状态变化通知，closed仍不提交。
    queue->closed = 1; // 两类通知成功后在同一guard下提交关闭。
    return release(queue, 0); // 醒来者拿到guard以后看到最终状态。
}
