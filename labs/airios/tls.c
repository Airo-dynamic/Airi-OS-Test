#include <airi_libc.h> // FS基址由自有系统调用设置；内存仍由用户页接口持有。
static uint64_t read_self(void) { // offset0存本地块自己的用户VA。
    uint64_t value; // 结果进入普通寄存器，不返回内核指针。
    __asm__ volatile("movq %%fs:0,%0" : "=r"(value) :: "memory"); // 地址是FS.base加0，再走当前用户页表。
    return value; // 这是本节自定的小块布局，尚非C运行库的TCB ABI。
}
static uint64_t read_value(void) { // 同一条指令在不同基址读取不同用户对象。
    uint64_t value; // 实际FS访问的值。
    __asm__ volatile("movq %%fs:8,%0" : "=r"(value) :: "memory"); // offset8保存当前身份的可辨识数据。
    return value; // 不用C数组下标冒充FS硬件寻址。
}
static void write_value(uint64_t value) { // 仍受当前用户映射的可写权限检查。
    __asm__ volatile("movq %0,%%fs:8" :: "r"(value) : "memory"); // 基址不变，数据写入本进程页面。
}
static int spin(uint64_t self, uint64_t expected) { // 不在循环中主动yield或进入内核。
    for (unsigned i = 0; i < 5000000; ++i) // 有界纯计算，PIT可在任意一轮抢占。
        if (read_self() != self || read_value() != expected) // 两个实际FS偏移都必须来自本进程。
            return 1; // 捕获另一个进程基址或遗漏恢复。
    return 0; // 本轮每次实际访问均符合当前身份。
}
int main(void) { // 每个普通静态ELF仍从FS=0开始，本节手工建立本地块。
    if (airi_fs_base()) // 前一个命令或shell不得把旧基址传给新ELF。
        return 192; // 不在未知环境中继续。
    uint64_t *page = airi_alloc(4096); // 页面所有权属于本进程，基址设置不会另行分配它。
    if (!page) // 明确处理资源失败。
        return 193; // 尚未改变FS。
    uint64_t *local = page + 8; // 从页内偏移64开始，FS基址不要求按页对齐。
    local[0] = (uintptr_t)local; local[1] = 42; // 0为self，8为value，其他字段尚不使用。
    if (airi_fs_set((uintptr_t)local) || airi_fs_base() != (long)(uintptr_t)local || read_self() != (uintptr_t)local || read_value() != 42) // 设置、查询和硬件访问分别核对。
        return 194; // 数值相同不足以替代实际寻址。
    for (unsigned i = 0; i < 3; ++i) { // 中间非规范地址、内核高半区与最大数值分别拒绝。
        uint64_t invalid[] = {0x0000800000000000ull, 0xffff800000000000ull, UINT64_MAX}; // 不让特权WRMSR替用户触发内核GP。
        if (airi_fs_set(invalid[i]) != -22 || airi_fs_base() != (long)(uintptr_t)local || read_value() != 42) // 失败必须保持旧基址和数据。
            return 195; // 参数失败不能污染后续返回路径。
    }
    __asm__ volatile("movw $35,%%ax; movw %%ax,%%fs" ::: "rax", "memory"); // 用户可装载合法数据段选择子，这会把实际FS基址改为该描述符的0。
    if (airi_fs_base() != 0 || airi_fs_set((uintptr_t)local) || read_value() != 42) // 入口必须读真实硬件，不能只返回旧软件缓存。
        return 208; // 当前ABI返回前统一设空选择子，再写完整基址。
    if (airi_fs_set(0x12345000) || airi_fs_base() != 0x12345000 || airi_fs_set((uintptr_t)local)) // 合法低半区数值可尚未映射；未访问就不应产生PF。
        return 209; // 设置基址不是分配页或检查未来全部offset。
    long child = airi_fork(); // 基址数值与用户页面都继承，页面物理内容独立复制。
    if (child < 0) // 不能将资源失败当父分支继续。
        return 196; // 保留可诊断错误。
    if (airi_fs_base() != (long)(uintptr_t)local || read_value() != 42) // 父子都在改变自己的环境前核对继承。
        return 197; // 子必须先得到旧本地块的42。
    if (!child) { // 子换成另一个VA，以暴露只切CR3而遗漏FS恢复的实现。
        page = airi_alloc(4096); // 子另持一页，原页仍留给退出统一回收。
        if (!page) // 尚未修改当前继承值。
            return 198; // 分配失败可正常退出。
        local = page + 8; local[0] = (uintptr_t)local; local[1] = 99; // 与父的基址和数据都不同。
        if (airi_fs_set((uintptr_t)local)) // 完整低半区值只在返回用户前提交硬件。
            return 199; // 明确拒绝设置失败。
    }
    uint64_t expected = child ? 42 : 99, start = airi_ticks(); // 两个进程在真实时间窗口内竞争运行。
    do { // 循环内部不主动交出CPU，由PIT打断两种不同基址。
        if (spin((uintptr_t)local, expected) || airi_fs_base() != (long)(uintptr_t)local) // 活访问与系统调用返回后都要正确。
            return 200; // 不能只在某一种陷入路径恢复。
    } while (airi_ticks() - start < 20); // 至少二十个真实tick，非宿主墙钟猜测。
    write_value(expected + 1); // FS写也必须落在当前身份自己的页面。
    if (local[1] != expected + 1 || airi_exec("broken") != -8 || read_value() != expected + 1) // 失败exec保留FS与旧映像对象。
        return 201; // 不将默认FS=0错误套给失败事务。
    if (!child) { // 孩子用成功exec观察重新初始化。
        airi_puts("[TLS_CHILD_OK] fork=inherited base=private preempt=preserved failed_exec=preserved\n"); // 来自真实FS读写检查。
        airi_exec("tls-reset"); // 成功不回旧函数；旧页面与基址归属一起撤销。
        return 202; // 不用这条失败返回冒充新程序完成。
    }
    int status = 1; // 保存同一孩子在新ELF中的退出状态。
    if (airi_waitpid(child, &status) != child || status || read_value() != 43) // 父仍使用原本地块，子写100不能影响父。
        return 203; // 等待和切换都不能串状态。
    child = airi_fork(); // 独立孩子验证基址不固定其页面。
    if (child < 0) // 没有身份就不能执行预期故障。
        return 204; // 正常错误返回。
    if (!child) { // 基址仍指向自己的page+64。
        if (airi_release(page, 4096)) // 完整解除该页USER映射，基址数值仍保持。
            return 205; // 未释放就不能声称发生缺页。
        return (int)read_value(); // 实际FS:8触发PF14，不能进入内核访问旧物理页。
    }
    if (airi_waitpid(child, &status) != child || status != -142 || read_value() != 43) // 父的页和基址仍在。
        return 206; // 非当前故障不能击穿父或内核。
    if (airi_fs_set(0) || airi_fs_base() || airi_release(page, 4096)) // 先停用，再由原所有者释放本地块。
        return 207; // FS设置本身不做内存归还。
    airi_puts("[TLS_USER_OK] fork=inherited timer_window=20 fs=private invalid=unchanged exec=reset released=pf14\n"); // 完整生命周期结束后才发布。
    return 0; // shell继续接收命令，会话清理核对资源恢复。
}
