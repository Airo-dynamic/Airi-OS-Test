#include <airi_libc.h> // 阻塞由内核登记，用户不循环重试EAGAIN。
static void delay(void) { // 前两条子路径等待真实PIT，留出父亲登记阻塞的时间。
    uint64_t until = airi_ticks() + 5; // 约50毫秒，不把固定循环叫时钟。
    while (airi_ticks() < until) // 这里仅是作者子任务准备时序，父亲的read/write真正阻塞。
        __asm__ volatile("pause"); // CPU提示不等于内核睡眠。
}
int main(void) { // 依次验证读等待、写等待和其他用户纯计算时的超时。
    unsigned ends[2]; // 稳定FD表项在阻塞期间由本进程持有。
    char data[64], output[64]; // 复制发生在各自用户地址空间。
    if (airi_pipe(ends, 0)) // 本阶段新增阻塞模式0。
        return 90; // 不用未初始化FD。
    long child = airi_fork(); // 子进程会为父读请求提供事件。
    if (child < 0) // 明确容量失败。
        return 91; // 不进入无事件的等待。
    if (!child) { // 孩子只保留写端。
        airi_close(ends[0]); // 父亲仍持读端，不会EPIPE。
        delay(); // 父亲应在此期间进入USER_IO_WAIT。
        if (airi_write(ends[1], "awake", 5) != 5) // 写入改变条件，内核随后重试父请求。
            return 92; // 父wait可见错误。
        airi_close(ends[1]); // 最后写者结束，读完后EOF。
        return 0; // 不保留进程引用。
    }
    int status = -1; // 自有wait状态。
    if (airi_close(ends[1]) || airi_read(ends[0], output, 10) != 5 || memcmp(output, "awake", 5) || airi_waitpid(child, &status) != child || status || airi_close(ends[0])) // 原read一次调用返回真实前缀，不是用户重试循环。
        return 93; // 任一边界失败都不发布成功。
    memset(data, 'a', sizeof(data)); // 第二条流先填满。
    if (airi_pipe(ends, 0) || airi_write(ends[1], data, sizeof(data)) != 64) // 父亲下一次写需要等读者释放空位。
        return 94; // 不能凭空成功。
    child = airi_fork(); // 孩子本次只保留读端。
    if (child < 0) // 同样处理资源错误。
        return 95; // 不等不存在消费者。
    if (!child) { // 延后读取，保证父在满流阻塞。
        airi_close(ends[1]); // 父仍有写端。
        delay(); // 留出父写请求登记时间。
        if (airi_read(ends[0], output, 32) != 32 || airi_read(ends[0], output, 33) != 33 || output[32] != 'x') // 第一读释放空位，唤醒父写x，再读到余下32个a和x。
            return 96; // 真正顺序取决于内核完成父请求，不是共享用户数组。
        airi_close(ends[0]); // 消费结束。
        return 0; // 父wait取得0。
    }
    if (airi_close(ends[0]) || airi_write(ends[1], "x", 1) != 1 || airi_waitpid(child, &status) != child || status || airi_close(ends[1])) // 单次满流write必须阻塞后完成，而非返回EAGAIN。
        return 97; // 调用顺序可连续追踪。
    if (airi_pipe(ends, 1)) // 此时没有孩子，空流等待将让全部用户都阻塞。
        return 103; // 保留写端避免EOF。
    uint64_t idle_before = airi_ticks(); // 真实PIT作为空闲唤醒来源。
    if (airi_wait_ready(ends[0], AIRI_READABLE, 3) != 0 || airi_ticks() - idle_before < 3 || airi_close(ends[0]) || airi_close(ends[1])) // 启动服务栈必须hlt并在PIT后重查截止。
        return 104; // 不能因没有其他用户就PANIC或伪完成。
    airi_puts("[WAIT_IDLE_OK] all_users=blocked ticks>=3\n"); // 来自只剩当前用户的真实等待。
    if (airi_pipe(ends, 1) || airi_wait_ready(ends[0], AIRI_READABLE, 0) != 0) // 就绪0tick只是查询，不阻塞。
        return 98; // 空流但有写端，条件不成立。
    child = airi_fork(); // 另一用户将长时间纯计算，不调用ticks或yield。
    if (child < 0) // 不丢容量错误。
        return 99; // 退出前由内核收拢FD。
    if (!child) { // 纯整数汇编不触碰未支持的SIMD现场。
        airi_close(ends[0]); // 只保留将来写入的端点。
        __asm__ volatile("mov $200000000, %%rcx; 1: dec %%rcx; jnz 1b" ::: "rcx", "cc"); // 有界计算用于让另一个用户持续占CPU，不当作时间测量。
        if (airi_write(ends[1], "z", 1) != 1) // 计算结束才提供数据。
            return 100; // 父亲随后验证结果。
        airi_puts("[WAIT_BUSY_DONE]\n"); // 应在父超时标记之后。
        airi_close(ends[1]); // 父排空后EOF。
        return 0; // 正常结束。
    }
    airi_close(ends[1]); // 空流唯一写者是忙算孩子。
    uint64_t before = airi_ticks(); // 计时来自PIT事件，不来自循环步数。
    if (airi_wait_ready(ends[0], AIRI_READABLE, 3) != 0 || airi_ticks() - before < 3 || airi_read(ends[0], output, 1) != -11) // 超时必须在孩子真正写入前发生，即使孩子不进系统调用。
        return 101; // 否则内核延后服务可能被纯计算用户饿死。
    airi_puts("[WAIT_TIMEOUT_OK] other_user=busy ticks>=3\n"); // 实际唤醒发生在同一次wait调用之后。
    if (airi_waitpid(child, &status) != child || status || airi_read(ends[0], output, 1) != 1 || output[0] != 'z' || airi_read(ends[0], output, 1) != 0 || airi_close(ends[0])) // 等忙算用户正常完成并排空真实字节。
        return 102; // 不因超时破坏管道或进程。
    airi_puts("[WAIT_USER_OK] read=blocked write=blocked readiness=timeout continuation=preserved\n"); // 全部等待与资源路径完成。
    return 0; // 会话统计还会核对实际阻塞/唤醒次数。
}
