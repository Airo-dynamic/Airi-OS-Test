#include <stdlib.h> // 本ELF显式选择对象分配实现。
#include <airi_malloc.h> // 观察请求对象与页面缓存，不猜物理页状态。
#include <airi_libc.h> // 进程、原始页接口与串口输出。
#include <errno.h> // 用户库失败使用真实TLS错误槽。
static int info(size_t arena, size_t bytes, size_t count) { // 对照三种独立数量。
    struct airi_malloc_info value = airi_malloc_info(); // 查询不会首次申请arena。
    return value.arena_bytes == arena && value.live_bytes == bytes && value.live_objects == count; // 不把缓存误记为活对象。
}
int main(int argc, char **argv) { // 同一ELF另以heap-reset验证成功exec的全新BSS。
    const size_t arena = 64 * 4096; // 当前用户分配器的明确容量。
    if (!info(0, 0, 0) || errno) // 进入main前不能已经分配arena或继承旧错误槽。
        return 31; // exec重建失败不能由新分配掩盖。
    if (argc == 1 && strlen(argv[0]) == 10 && !memcmp(argv[0], "heap-reset", 10)) { // 孩子成功替换到同一ELF的新实例。
        unsigned char *value = calloc(97, 1); // 新arena应从真实页接口取得。
        if (!value || value[0] || value[96] || !info(arena, 97, 1)) // 新对象和计数都来自本次执行。
            return 32; // 不能沿用旧块链地址而只把计数清零。
        free(value); // 页面留作缓存，退出时由内核归还。
        if (!info(arena, 0, 0)) // 无活对象不等于未持有arena。
            return 33; // 不能把free假装成release。
        airi_puts("[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero\n"); // 真正exec后才发布。
        return 0; // 父等待这个实际身份。
    }
    errno = EBUSY; // 零操作都应保留旧错误。
    free(NULL); // 不得初始化arena或修改错误。
    if (malloc(0) || calloc(0, SIZE_MAX) || calloc(SIZE_MAX, 0) || realloc(NULL, 0) || errno != EBUSY || !info(0, 0, 0)) // 零请求采用本实现明确的NULL规则。
        return 34; // 不用NULL本身推断ENOMEM。
    if (calloc(SIZE_MAX, 2) || errno != ENOMEM || malloc(SIZE_MAX) || errno != ENOMEM || !info(0, 0, 0)) // 先拒绝无法表示的乘法和明显超限大小。
        return 35; // 不能为回绕后的小请求建立arena。
    void *pages[80]; size_t lengths[80], count = 0; // 保存每批原始页面的独立归还长度，最多十五大批和六十三单页。
    for (size_t step = arena; step; step = step == arena ? 4096 : 0) { // 先填64页批次，再填最后不足64页的预算。
        for (;;) { // 直到当前批次再也无法取得。
            void *page = airi_alloc(step); // 原始页接口没有调用对象分配器。
            if (!page) // 当前步长耗尽；大批失败后还应尝试单页。
                break; // 不把失败指针写进归还列表。
            if (count == 80) // 防止以后扩大预算时悄悄覆盖用户栈。
                return 36; // 当前固定1024页预算不应到达。
            pages[count] = page; lengths[count++] = step; // 保存实际成功的整批所有权。
        }
    }
    if (malloc(1) || errno != ENOMEM || !info(0, 0, 0)) // 没有数据页时首次arena申请真实失败，描述仍为空。
        return 37; // 不用纯大小检查代替真实内核失败。
    while (count) { // 完整归还自己持有的原始页。
        --count; // 从最后一个有效表项开始。
        if (airi_release(pages[count], lengths[count])) // 每段保持原始页对齐与长度。
            return 38; // 失败不能继续伪装已经恢复预算。
    }
    for (size_t bytes = 1; bytes <= 65; ++bytes) { // 覆盖不同补齐余数，并重复复用同一arena。
        unsigned char *value = malloc(bytes); // 内核失败恢复后此处必须可以重试。
        if (!value || (uintptr_t)value % 16 || !info(arena, bytes, 1)) // 当前目标基本对齐至少16字节。
            return 39; // 不把请求头大小遗漏在对齐中。
        memset(value, 0xa5, bytes); free(value); // 下一次calloc必须清除这类复用内容。
    }
    unsigned char *zero = calloc(65, 1); // 首次适配应复用之前写过的空块。
    if (!zero) // 后续字节检查只能针对成功对象。
        return 40; // 不能假定calloc必然成功。
    for (unsigned i = 0; i < 65; ++i) // 检查完整请求，而非只看第一页或首字节。
        if (zero[i]) // 复用内容也须为零。
            return 41; // 页最初清零不能代替每次calloc清零。
    free(zero); // 恢复无活对象状态。
    void *left = malloc(70000), *right = malloc(70000); // 两个相邻活块后仍有空闲尾块。
    if (!left || !right) // 先确认真实分裂出两块。
        return 42; // 不拿NULL参与复用判断。
    free(left); free(right); // 连续空块及它们的内部块头应合并。
    void *merged = malloc(180000); // 单一旧块均不够，合并后才有足够连续空间。
    if (!merged || !info(arena, 180000, 1)) // 不用空闲容量之和冒充连续可分配范围。
        return 43; // 相邻空块合并必须实际发生。
    free(merged); // 为正常realloc恢复空间。
    unsigned char *old = malloc(33), *guard = malloc(97); // 同时活着的邻居用于观察复制范围。
    if (!old || !guard) // 不在失败指针上填充。
        return 44; // 留存具体故障位置。
    for (unsigned i = 0; i < 33; ++i) // 每个旧字节可辨认。
        old[i] = (unsigned char)(i ^ 0x59); // 扩容必须保留完整前缀。
    memset(guard, 0x6d, 97); // 另一对象不能被复制或free影响。
    uintptr_t old_address = (uintptr_t)old; // 先记录地址数字，成功搬运后不再读取已结束生命周期的旧指针。
    unsigned char *grown = realloc(old, 8193); // 先拿临时返回值；失败时old仍有效。
    if (!grown || (uintptr_t)grown == old_address || !info(arena, 8193 + 97, 2)) // 本实现增长总是搬运到新块。
        return 45; // 对照请求字节，不能按对齐容量统计。
    for (unsigned i = 0; i < 33; ++i) // 只比较旧请求范围，新尾部没有初始化保证。
        if (grown[i] != (unsigned char)(i ^ 0x59)) // 顺序与内容必须保留。
            return 46; // 不读取已结束生命周期的old。
    if (realloc(grown, 17) != grown || !info(arena, 17 + 97, 2)) // 缩小不搬运，但改变请求计数。
        return 47; // 底层容量暂不缩小，不得在此读旧尾部。
    for (unsigned i = 0; i < 97; ++i) // 邻居仍存活。
        if (guard[i] != 0x6d) // 扩容/缩小都不得污染它。
            return 48; // 不只检查分配器自身计数。
    errno = EBUSY; // free与零长度realloc不清旧错误。
    if (realloc(grown, 0) || errno != EBUSY) // 本实现明确结束旧对象并返回NULL。
        return 49; // 不把此NULL写成内存不足。
    free(guard); // 恢复整个arena可复用。
    unsigned char *large = malloc(180000); // 稍后扩容需要同时持有新旧大块。
    void *blocker = malloc(60000); // 剩余空间不足以先放下190000字节的新对象。
    if (!large || !blocker) // 两项实际成功后才开始失败观察。
        return 50; // 不用超大无效请求替代容量耗尽。
    memset(large, 0x37, 180000); // 整个旧范围都须在失败后可访问。
    if (realloc(large, 190000) || errno != ENOMEM || !info(arena, 240000, 2)) // 峰值容量真实不足，旧对象不能被释放或缩短。
        return 51; // 调用者仍持有large。
    for (size_t i = 0; i < 180000; ++i) // 检查完整旧对象。
        if (large[i] != 0x37) // 失败不能破坏旧内容。
            return 52; // 此后仍能正常free。
    free(blocker); free(large); // 相邻空块重新合并。
    for (unsigned round = 0; round < 128; ++round) { // 反复使用全部大空间，不依赖进程退出恢复。
        void *value = malloc(250000); // 在同一arena内接近上限，但仍容得下元数据。
        if (!value) // 历史块头不能逐轮吃掉容量。
            return 53; // 检查反复释放与合并。
        free(value); // 每轮结束活字节均回到零。
    }
    unsigned char *inherited = malloc(97); // fork前保留一个真实活对象与块链。
    if (!inherited) // 后续只操作成功借出的对象。
        return 54; // 不在NULL上写入。
    memset(inherited, 0x61, 97); // 同虚拟地址在父子中应指向各自物理页。
    long child = airi_fork(); // 当前直接复制用户BSS、arena页和块头，指针数值保持。
    if (child < 0 || !info(arena, 97, 1)) // 父子均应继承同样的请求状态。
        return 55; // fork不是从零模板建立分配器。
    if (!child) { // 孩子只改变自己的对象及分配器元数据。
        inherited[0] = 0x72; // 父的相同VA内容不得变化。
        if (airi_exec("broken") != -8 || inherited[0] != 0x72 || !info(arena, 97, 1)) // 失败exec完整保留原arena与块链。
            return 56; // 不能只保存返回码而丢失页所有权。
        free(inherited); // 子归还同VA对象不影响父的借用。
        if (!info(arena, 0, 0)) // 缓存仍在旧孩子映像内。
            return 57; // 成功exec应由内核回收它。
        airi_exec("heap-reset"); // 同一PID进入新BSS与TLS，旧arena映射结束。
        return 58; // 成功不会回到旧调用点。
    }
    int status = 1; // 父实际等待孩子的新映像退出。
    if (airi_waitpid(child, &status) != child || status || !info(arena, 97, 1)) // 子free/exec/exit不能改变父对象计数。
        return 59; // wait的真实结果必须成功。
    for (unsigned i = 0; i < 97; ++i) // 子写过的首字节以及所有其他内容仍由父持有。
        if (inherited[i] != 0x61) // 父子物理页保持隔离。
            return 60; // 不把指针数值相同误认为共享对象。
    free(inherited); // 父最后一个对象归还。
    if (!info(arena, 0, 0)) // 活对象清零，arena等待进程退出由内核回收。
        return 61; // 串口成功标记不应掩盖剩余借用。
    airi_puts("[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset\n"); // 所有正常与失败路径实际完成。
    return 0; // shell继续运行，整会话收尾另核对用户页和内核资源。
}
