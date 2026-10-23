#include <airi_libc.h> // 监督程序在ring3，内核只提供进程、等待与唯一UART消费身份。
int main(void) { // 明确run init后常驻，不用循环演示次数冒充永久服务。
    if (airi_sleep(0) || airi_sleep(6001) != -22) // 零长度与超出6000tick上限都不能意外阻塞。
        return 141; // ABI边界错误不继续提供错误服务。
    airi_puts("[INIT_START] service=sh input=UART restart_delay=100\n"); // 实际用户代码已经开始执行。
    for (;;) { // init持续监督，不随某次shell退出结束。
        long child = airi_fork(); // 新孩子继承输出，但init自身没有UART输入FD。
        if (!child) { // 只有孩子替换映像，监督者保留原代码和wait位置。
            airi_exec("sh"); // 内核只给init直接启动的sh授予前台UART输入。
            airi_puts("[INIT_EXEC_FAILED]\n"); // exec返回意味着明确失败。
            airi_exit(142); // 父取得失败状态后按同样退避策略重试。
        }
        if (child < 0) // 表满或资源耗尽不允许快速重试挤占CPU。
            airi_puts("[INIT_SPAWN_FAILED]\n"); // 本轮没有可wait的孩子。
        else { // 只有实际创建的身份才进入wait。
            int status = -1; // 由同一次wait写入孩子退出结果。
            if (airi_waitpid(child, &status) != child) // 不把未知wait失败当作正常logout。
                return 143; // 监督者内部关系被破坏，明确结束并交回监控台。
            airi_puts(status ? "[INIT_REAP] result=failed\n" : "[INIT_REAP] result=normal\n"); // 状态在释放孩子资源前已由内核交付。
        }
        uint64_t start = airi_ticks(); // 重启退避使用单调tick，不使用墙钟。
        if (airi_sleep(100) || airi_ticks() - start < 100) // 真正离开可运行集合至少100个PIT事件。
            return 144; // 提前恢复或调用错误不能继续快速重启。
        airi_puts("[INIT_RESTART] delay_elapsed=1\n"); // 只有等待截止已满足才开始下一次fork。
    }
}
