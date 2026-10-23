#include <airi_libc.h> // 用户只能通过既有进程、管道和时钟ABI观察托管。
static void pass_ticks(unsigned count) { // 给其他用户真实时间片，不拿主机sleep冒充guest执行。
    uint64_t start = airi_ticks(); // PIT提供单调事件基准。
    while (airi_ticks() - start < count) // 等待本轮有界时间，不依赖CPU执行速度。
        airi_yield(); // 让已就绪的孙进程完成退出与延后清理。
}
static int round_trip(unsigned finished) { // 两条路径分别覆盖先退出的僵尸和仍在等待的孤儿。
    unsigned gate[2], report[2]; // gate控制放行，report只传本轮一个完成字符。
    if (airi_pipe(gate, 0) || airi_pipe(report, 0)) // 一次最多四个额外FD，所有失败都以退出收拢。
        return 101; // 不用未初始化端点继续fork。
    long middle = airi_fork(); // 根持久运行，中间父进程将在本轮提前退出。
    if (middle < 0) // 槽泄漏会使后续轮次在此失败。
        return 102; // 不重试掩盖资源耗尽。
    if (!middle) { // 中间父进程拥有独立页，管道对象仍共享。
        airi_close(gate[1]); // 只有根负责放行。
        airi_close(report[0]); // 只有根读取报告。
        long leaf = airi_fork(); // 真正需要托管的孙进程。
        if (leaf < 0) // 上一轮没回收将逐渐耗尽八槽表。
            airi_exit(103); // 根wait取得明确错误。
        if (!leaf) { // 孙进程不会被根的waitpid直接回收。
            char value = 'L'; // 活孤儿得到放行后报告。
            if (!finished && airi_read(gate[0], &value, 1) != 1) // 等根已经wait中间父后才继续，确保发生活孤儿交接。
                airi_exit(104); // 不能把管道错误当放行。
            value = finished ? 'Z' : 'L'; // 区分两种实际运行路径。
            if (airi_write(report[1], &value, 1) != 1) // 一字节报告，不假设父退出会杀孩子。
                airi_exit(105); // 短写或错误明确失败。
            airi_exit(17); // 故意不由中间父wait，测试内核托管。
        }
        if (finished) { // 让孙先退出为僵尸，中间父再退出。
            char release; // 根先观察报告与时间片，再写放行字符。
            if (airi_read(gate[0], &release, 1) != 1) // 真阻塞，不能在孙之前抢先结束。
                airi_exit(106); // 错误不继续发布正常退出码。
        }
        airi_exit(31); // 不wait孙，FD由exit关闭，孙的引用仍有效。
    }
    airi_close(gate[0]); // 根不读取放行管道。
    airi_close(report[1]); // 根不持有报告写端，能正确看到异常EOF。
    char value = 0; // 只接受本轮实际报告。
    if (finished) { // 先确保孙已运行，再让中间父退出。
        if (airi_read(report[0], &value, 1) != 1 || value != 'Z') // 报告来自孙的用户代码。
            return 107; // 不能用旧字节满足本轮。
        pass_ticks(4); // 两个时间片让报告后的exit实际执行，作者同时核对finished=1记录。
        if (airi_write(gate[1], "!", 1) != 1) // 放行中间父，不影响已经结束的孙。
            return 108; // 无读者等错误不能忽略。
    }
    int status = -1; // 中间父自己的退出结果。
    if (airi_waitpid(middle, &status) != middle || status != 31) // 根只能wait直接孩子，不知道孙的PID也能继续。
        return 109; // 子孙错误和槽耗尽使本轮明确失败。
    if (!finished) { // 中间父确认退出后，孙必须仍可继续执行。
        if (airi_write(gate[1], "!", 1) != 1 || airi_read(report[0], &value, 1) != 1 || value != 'L') // 管道引用跨父退出保持。
            return 110; // 活孤儿未正确保留运行或等待状态。
    }
    airi_close(gate[1]); // 根不保留跨轮引用。
    airi_close(report[0]); // 所有报告字节已消费。
    pass_ticks(4); // 孤儿退出后给内核下一条安全栈回收机会。
    if (airi_waitpid(-1, &status) != -10) // 托管者是内核PID0，不偷偷把孙变成根的可wait孩子。
        return 111; // 根的普通父子语义必须保持。
    return 0; // 此时根会话仍存活，尚未触发launch末尾的统一回收。
}
int main(void) { // 不依赖新系统调用，复用K33前已经实现的用户库。
    for (unsigned i = 0; i < 24; ++i) { // 共二十四轮，远超过八槽表能靠一次性分配承受的数量。
        int error = round_trip(i & 1u); // 交替验证活孤儿与已结束僵尸。
        if (error) // 第一次失败立即给shell完整退出码。
            return error; // 不用重启清空资源后假装持续运行。
    }
    airi_puts("[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive\n"); // 只有同一根会话连续完成才出现。
    return 0; // launch最后再核对全部页面、FD与异步静态池归还。
}
