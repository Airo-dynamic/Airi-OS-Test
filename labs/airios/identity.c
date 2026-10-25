#include <airi/runtime_abi.h> // 内核和用户约定的16字节运行头。
#include <airi_threads.h> // 快速身份与工作者生命周期。
#include <airi_mutex.h> // 不争用锁也应复用本地身份读取。
#include <airi_libc.h> // 真实进程、PIT和串口调用。
static _Thread_local volatile unsigned local = 42; // fork复制运行值，exec和新worker恢复声明初值。
static void require(int condition, const char *message) { // 失败终止所属真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(84); } // 不让错误路径输出成功标记。
}
static struct airi_runtime_tcb *control(void) { // 只在内核建立的静态TLS布局下取头。
    struct airi_runtime_tcb *value; // 指向当前执行者的用户映射，不跨执行者保存。
    __asm__ volatile("movq %%fs:0,%0" : "=r"(value) :: "memory"); // 已有self槽提供TCB自身用户VA。
    return value; // 后续字段仍通过当前用户页表访问。
}
static long raw(long number) { // 绕开库包装，用实际内核查询校准身份和fork。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(0ull), "S"(0ull), "d"(0ull) : "memory", "cc"); // 参数为0，编号42查询或6复制进程。
    return number; // 参考值由内核真实运行者产生。
}
static long inspect(void) { // 每次恰做一次原始42号，其他身份读取应留在用户态。
    struct airi_runtime_tcb *value = control(); // 当前有效TP由内核在发布执行者以前建立。
    long actual = raw(42); // 内核日志记录真实查询，不能只用用户计数推断没陷入。
    require(value->self == (uintptr_t)value && value->version == AIRI_RUNTIME_VERSION && value->tid == (uint32_t)actual, "published TCB identity"); // 未更新的fork/exec候选0会在这里失败。
    for (unsigned i = 0; i < 128; ++i) require(airi_thread_id() == actual, "fast identity stable"); // 应只有TCB读，不重复42号查询。
    airi_mutex own = AIRI_MUTEX_INITIALIZER; // 当前调用栈独有，不制造另一项竞争。
    uint64_t before = airi_thread_syscalls(); // 记录当前执行者计数，不用全局计数混入其他工作者。
    for (unsigned i = 0; i < 8; ++i) require(airi_mutex_lock(&own, 0) == 0 && airi_mutex_unlock(&own) == 0, "mutex uses local identity"); // 有效TCB且无人参与竞争，八对操作应全部留在用户态。
    uint64_t after = airi_thread_syscalls(); // 结束查询自身也增加一次，不能当作锁的开销。
    require(after - before == 1, "uncontended mutex skips notification"); // 只有结束查询45的一次，八对锁不额外发出请求。
    airi_puts("[MUTEX_PROFILE] loops=8 delta=1\n"); // 只有实际内核计数差符合才打印，不从函数名猜次数。
    return actual; // 后续用独立原始参考比较，不自证同一缓存。
}
static long worker(void *argument) { // 原始声明值与本次内核TID都须属于新执行者。
    unsigned index = (unsigned)(uintptr_t)argument; // 由主执行者提供0、1或2。
    require(local == 42, "worker fresh TLS"); // 不继承主执行者改成99的运行值。
    require(airi_thread_syscalls() == 1, "new worker syscall counter"); // 工作入口没有继承创建者的累计值。
    long identity = inspect(); // 该worker恰有一次真实参考查询。
    require(identity != airi_getpid(), "worker TID differs from shared PID"); // 同进程身份与执行者身份分别判断。
    local = 200 + index; // 每个实际TLS副本保留不同观察值。
    uint64_t start = airi_ticks(); // 后面的循环必须跨真实PIT窗口。
    do { // 不用主动yield代替身份隔离验证。
        for (unsigned i = 0; i < 20000; ++i) // 在用户态反复读运行头和真实TLS。
            if (airi_thread_id() != identity || local != 200 + index) return -211; // 换执行者后不能读到另一人的头或变量。
    } while (airi_ticks() - start < 4); // 至少四个全局事件，每位worker的用户tick另由宿主日志核对。
    return identity; // join保留完整long，便于对应实际创建TID。
}
static int named(const char *value, const char *expected) { // 启动参数只包含初始目录名称。
    size_t bytes = strlen(expected); // 先取得固定短名称的长度。
    return strlen(value) == bytes && !memcmp(value, expected, bytes); // 不借宿主字符串库。
}
int main(int argc, char **argv) { // exec前后使用同一ELF的不同目录名。
    require(argc == 1, "identity argv"); // 最小启动协议仍是单项程序名。
    if (named(argv[0], "identity-reset")) { // 成功exec重新建立TLS与运行头，PID仍为原孩子。
        require(airi_thread_syscalls() > 1, "exec keeps execution counter"); // 新main不等于新执行者，exec自身和此前请求继续累计。
        require(local == 42 && inspect() == airi_getpid(), "exec fresh TLS and retained PID"); // 不能把候选的0或旧运行值123带入新main。
        airi_puts("[IDENTITY_EXEC_OK] tls=42 tcb=current version=1\n"); // 原始参考查询与字段均已经检查。
        return 0; // 父wait取得成功结果。
    }
    require(airi_thread_syscalls() >= 1, "primary syscall counter"); // 经shell的exec进入时已经有请求，不能硬编码初始值1。
    long parent = inspect(); // 主执行者版本头在首次进入用户前即有真实TID。
    require(parent == airi_getpid(), "primary TID"); // 主执行者初始TID等于PID。
    struct airi_runtime_tcb *value = control(); // 本阶段只在无worker时修改自己的版本，观察后立即恢复。
    value->version = 0; // 模拟旧版保留头，不破坏self或映射。
    require(airi_thread_id() == parent, "old-version fallback"); // 这次必须真的进入42号，而不是返回0或伪造值。
    value->version = AIRI_RUNTIME_VERSION; // 后续再次使用用户态快路径。
    local = 99; // fork应继承这份运行值，但运行头TID必须改变。
    long child = raw(6); // 直接系统调用，证明身份更新不依赖库的fork回调。
    require(child >= 0, "raw fork"); // 失败不进入孩子逻辑。
    if (!child) { // 新地址空间保留调用继续点和普通TLS运行值。
        require(airi_thread_syscalls() == 1, "fork child starts counter"); // 孩子恢复父调用点，但没有实际进入父的6号请求。
        require(local == 99 && inspect() == airi_getpid() && airi_thread_id() != parent, "raw fork child identity"); // 只有TCB身份被内核改为孩子。
        local = 123; // 与随后exec的声明初值区别。
        airi_exec("identity-reset"); // 成功进入新main，保留同一孩子PID。
        require(0, "identity exec returned"); // 返回只可能是失败。
    }
    int status; // 与工作函数long返回值分开。
    require(airi_waitpid(child, &status) == child && !status && local == 99 && airi_thread_id() == parent, "parent TLS unchanged"); // 子写入不污染父头和变量。
    long first = airi_thread_create(worker, (void *)(uintptr_t)0), second = airi_thread_create(worker, (void *)(uintptr_t)1), result; // 两条计算执行者交替取得PIT。
    require(first > 0 && second > 0, "identity workers"); // 不用单执行者代替隔离。
    require(airi_thread_join(first, &result) == 0 && result == first && airi_thread_join(second, &result) == 0 && result == second, "identity worker results"); // 返回值对应真实TID。
    long reused = airi_thread_create(worker, (void *)(uintptr_t)2); // 相同槽重用时必须重新写新身份。
    require(reused > 0 && reused != first && reused != second && airi_thread_join(reused, &result) == 0 && result == reused, "identity reused slot"); // 不能沿用原槽TCB中的旧TID。
    airi_puts("[IDENTITY_USER_OK] tcb=version1 fast=local fork=raw exec=fresh workers=3 reused=fresh fallback=queried\n"); // 所有真实身份路径完成后才输出。
    return 0; // 用户shell收取本条命令成功。
}
