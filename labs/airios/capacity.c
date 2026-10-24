#include <airi_libc.h> // 大映像仍只调用自有用户ABI，不借宿主运行时。
static volatile unsigned char data = 0x53; // 文件中的初始化字节与BSS分别检查。
static volatile unsigned char bss[384 * 1024]; // 超过旧六十四页总预算，ELF文件不保存这些零字节。
__attribute__((noinline)) static int stack_and_fork(void) { // 保留真实函数帧，不能把大数组优化掉。
    volatile unsigned char local[32768]; // 使用八页以上用户栈，仍小于固定64KiB范围。
    for (unsigned i = 0; i < sizeof(local); ++i) // 每个字节实际触发用户写入。
        local[i] = (unsigned char)(i ^ 0x5a); // 页内位置变化使内容检查不只看首字节。
    long child = airi_fork(); // 此时父拥有1024数据页，孩子复制全部七页元数据所描述的映射。
    if (child < 0) // 普通资源失败不能继续按孩子身份处理。
        return 151; // 交给shell保留精确错误。
    if (!child) { // 子与父同VA，但物理数据和记录页各自拥有。
        bss[0] = 0x99; // 改变子BSS不应影响父的0x31。
        if (airi_exec("broken") != -8 || bss[0] != 0x99 || local[12345] != (unsigned char)(12345 ^ 0x5a)) // 失败exec保持旧大映像与跨页栈。
            return 152; // 不把拒绝坏ELF误认为成功替换。
        airi_exec("worker"); // 成功时同PID进入worker，旧1024页和七个记录页归还。
        return 153; // 成功exec不能返回这里。
    }
    int status = 0; // worker的普通退出值仍由父wait取得。
    if (airi_waitpid(child, &status) != child || status != 23 || bss[0] != 0x31) // 子exec和exit不能释放父页面。
        return 154; // 身份、状态、父数据必须同时保持。
    for (unsigned i = 0; i < sizeof(local); ++i) // wait后仍沿原用户栈继续执行。
        if (local[i] != (unsigned char)(i ^ 0x5a)) // fork/exec内核操作不能覆盖用户栈内容。
            return 155; // 捕获栈映射或复制错误。
    return 0; // local的C对象寿命结束，页面仍属于初始用户栈。
}
static int fill_and_release(void) { // 一次完整耗尽与恢复，后面在同一进程重复。
    unsigned char *blocks[1024]; // 记录用户动态范围，不读取内核页预算或内部指针。
    unsigned sizes[1024]; // 每项保存本次实际申请长度，归还时保持原范围。
    unsigned count = 0, pages = 0; // 按成功返回累计，不假定ELF段页数。
    for (unsigned step = 64; step; step = step == 64 ? 1 : 0) { // 先用256KiB块，再用单页填满剩余不足一块的容量。
        while (count < 1024) { // 明确保护本地记录数组，内核耗尽通常更早到达。
            unsigned char *block = airi_alloc(step * 4096u); // 失败NULL不交付半个范围。
            if (!block) // 当前块大小放不下，转成下一种更小请求或结束。
                break; // 不把失败地址记录为活块。
            for (unsigned i = 0; i < step * 4096u; ++i) // 检查所有新字节，包括跨页部分。
                if (block[i]) // 重复轮次旧填充值必须被清零。
                    return 156; // 只在完整清零后使用。
            memset(block, 0xa6, step * 4096u); // 留下跨页可辨识内容。
            blocks[count] = block; // 有效用户VA仍由本进程拥有。
            sizes[count++] = step * 4096u; // 同一下标记录长度，再发布一项。
            pages += step; // 栈和ELF另占预算，不计入这里。
        }
    }
    if (pages < 512 || count == 1024 || airi_alloc(4096) || airi_alloc(0) || airi_alloc(262145)) // 至少2MiB动态页，最终单页也耗尽；零长和过大请求拒绝。
        return 157; // 不拿旧六十四页限制冒充扩容完成。
    if (data != 0x53 || bss[0] != 0x31 || bss[sizeof(bss) - 1] != 0x31) // 耗尽失败不能改变已有ELF数据。
        return 158; // 分配请求失败后旧进程应继续可用。
    int error = stack_and_fork(); // 在满预算与真实大用户栈上进入系统调用。
    if (error) // 保存具体失败位置，不继续掩盖。
        return error; // 由内核退出清理尚持有的块。
    while (count) { // 逆序释放全部动态页，期间会越过多个元数据页边界。
        --count; // 退到最后一项仍持有的范围。
        if (blocks[count][0] != 0xa6 || blocks[count][sizes[count] - 1] != 0xa6 || airi_release(blocks[count], sizes[count])) // 邻居内容与整段归还都须成立。
            return 159; // 不将有损回收记录为成功。
    }
    return 0; // ELF和栈仍活着，下一轮再次填满同一地址空间。
}
int main(void) { // shell正常fork/exec进入真实大BSS映像。
    if (data != 0x53) // 初始化段必须来自文件内容。
        return 160; // 不继续掩盖装载错误。
    for (unsigned i = 0; i < sizeof(bss); ++i) // 验证全部384KiB零填充。
        if (bss[i]) // 大BSS与文件长度不同，不能越界复制文件。
            return 161; // 不只检查第一个页面。
    for (unsigned i = 0; i < sizeof(bss); ++i) // 写入父映像长期保留的内容。
        bss[i] = 0x31; // 子写0x99之后父仍须看到0x31。
    for (unsigned round = 0; round < 4; ++round) { // 不退出进程，反复耗尽、fork、exec、归还。
        int error = fill_and_release(); // 记录页也随尾部有效项减少而归还。
        if (error) // 任一轮失败保留普通退出码。
            return error; // shell可继续报告，而非触发内核PANIC。
    }
    long child = airi_fork(); // 单独验证固定用户栈下面没有被顺手映射。
    if (child < 0) // 身份创建失败不能冒充页故障。
        return 162; // 明确返回失败位置。
    if (!child) // 故障局限于这个孩子，父继续wait。
        return *(volatile unsigned char *)(uintptr_t)0x6fff0000; // 64KiB栈低端之下的未映射保护页，真实用户PF。
    int status = 0; // 故障编码沿已有用户wait ABI交付。
    if (airi_waitpid(child, &status) != child || status != -142) // 真实PF14，而非读出某个宿主预期值。
        return 163; // 错误映射或缺少隔离都会暴露。
    airi_puts("[USER_CAPACITY_OK] bss=393216 stack=32768 rounds=4 full_pages=1024 exec=worker guard=pf14\n"); // 所有路径实际完成后才发布。
    return 0; // shell继续接收其他命令，整次launch结束还会核对物理页回收。
}
