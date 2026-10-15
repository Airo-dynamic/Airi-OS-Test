#include <airi_libc.h> // 普通用户C程序，仅通过自有ABI请求进程服务。
static void busy_ticks(uint64_t count) { // 不主动yield，要求时钟真正能抢占用户执行。
    uint64_t start = airi_ticks(); // 单调事件基准。
    while (airi_ticks() - start < count) // 直到经历目标数量真实IRQ。
        for (volatile unsigned n = 0; n < 20000; ++n) // 每轮在用户态停留一段时间。
            __asm__ volatile("pause"); // 不调用内核分配或主动切换。
}
int main(int argc, char **argv) { // 同一个ELF入口会在fork后沿两个返回值分支继续。
    if (argc != 1 || !argv || !argv[0] || memcmp(argv[0], "processes", 10)) // 核对根程序的独立初始栈。
        return 81; // 参数错误不继续测试。
    if (airi_alloc(0) || airi_alloc(SIZE_MAX)) // 用户页请求的零长度和溢出大小必须失败。
        return 80; // 不能把无效请求当有效地址。
    unsigned char *value = airi_alloc(4096); // 一页零填充的用户内存，生命周期到进程退出。
    if (!value || value[0]) // 新页不允许泄漏旧所有者内容。
        return 82; // 申请失败或未清零。
    value[0] = 42; // fork前父拥有的内容。
    long child = airi_fork(); // 父得到正子PID，子得到0，失败为负数。
    if (child < 0) // 资源失败不能被当成父分支。
        return 83; // 明确退出失败。
    if (child == 0) { // 子的同一VA应映射到复制后的不同物理页。
        if (value[0] != 42) // 必须复制fork时的真实内容。
            return 84; // 不只复制寄存器。
        value[0] = 99; // 不能影响父页面。
        busy_ticks(5); // 给父进程机会进入真正阻塞的wait。
        airi_puts("[FORK_CHILD_OK] private_value=99\n"); // 真实用户输出。
        return 23; // 用户_start通过exit交回普通退出码。
    }
    const char *readonly = "read-only wait output"; // ELF只读段不能作为内核写结果的目标。
    if (airi_waitpid(child, (int *)(uintptr_t)readonly) != -14) // 内核应在阻塞和回收前拒绝不可写页。
        return 85; // 不能因错误输出指针丢失孩子状态。
    int status = -1; // 真正可写的父用户栈对象。
    if (airi_waitpid(child, &status) != child || status != 23 || value[0] != 42) // wait要交回子身份、退出码与独立父数据。
        return 86; // 任一错误都使根会话失败。
    if (airi_waitpid(child, &status) != -10) // 已回收孩子不能重复wait。
        return 87; // -ECHILD明确终止，不允许永久等待。
    long first = airi_fork(); // 再创建两个只做用户忙等的孩子。
    if (first < 0) // 保留资源失败语义。
        return 88; // 不假装任务已创建。
    if (!first) { // 第一个忙等孩子。
        busy_ticks(4); // 没有yield，必须让时间片转给其他用户。
        return 11; // 第一退出码。
    }
    long second = airi_fork(); // 父继续准备第二个孩子。
    if (second < 0) // 失败不是子分支。
        return 89; // 根演示明确失败。
    if (!second) { // 第二个忙等孩子。
        busy_ticks(4); // 两个不同根在真实IRQ边界交接。
        return 12; // 第二退出码。
    }
    if (airi_waitpid(first, &status) != first || status != 11) // 可以先等指定孩子，而非假设完成顺序。
        return 90; // 身份关联错误则失败。
    if (airi_waitpid(second, &status) != second || status != 12) // 再收另一个孩子，可能已是僵尸。
        return 91; // 同时覆盖阻塞和立即回收路径。
    long children[8]; // 父进程记录表满前实际创建的孩子身份。
    unsigned count = 0; // 只统计真正fork成功的项。
    for (unsigned i = 0; i < 8; ++i) { // 一个父加最多七个孩子，第八次必须明确失败。
        long pid = airi_fork(); // 孩子无论何时结束，在父wait前都占僵尸槽。
        if (!pid) // 新孩子不再参与父的创建循环。
            return 7; // 立即退出也不能让父尚未读取的状态丢失。
        if (pid < 0) { // 只接受预期的进程表满。
            if (pid != -11) // 不把其他错误伪装成容量边界。
                return 92; // 根演示失败。
            break; // 开始回收已创建的真实孩子。
        }
        children[count++] = pid; // 先保存身份，再增加等待数量。
    }
    if (count != 7) // 固定表八项应只给根之外七个槽。
        return 93; // 不允许静默扩展或提前失败。
    for (unsigned i = 0; i < count; ++i) // 逐个核对回收身份与普通退出码。
        if (airi_waitpid(children[i], &status) != children[i] || status != 7) // 可以覆盖阻塞和已结束两条wait路径。
            return 94; // 不能重复、遗漏或错配孩子。
    airi_puts("[PROCESS_FORK_WAIT_OK] copied_memory=private status=23,11,12 table_full=-11 reaped=7\n"); // 所有用户侧结果通过。
    return 0; // 根退出后内核验证整个会话资源恢复。
}
