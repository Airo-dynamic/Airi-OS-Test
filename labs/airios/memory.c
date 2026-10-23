#include <airi_libc.h> // 页生命周期仍通过自有ABI，不使用宿主malloc。
static int reuse_round(void) { // 每轮保持同一个进程地址空间，只替换动态页。
    unsigned char *a = airi_alloc(8192); // 两页对象，分配器从动态范围低端寻找空洞。
    unsigned char *b = airi_alloc(4096); // 紧随a的一页，之后用于扩大空洞。
    unsigned char *c = airi_alloc(4096); // 留住它，阻止两页请求跨过此页。
    if (!a || !b || !c || (uintptr_t)b != (uintptr_t)a + 8192 || (uintptr_t)c != (uintptr_t)a + 12288) // 固定工作负载核对最初布局。
        return 121; // 失败不能沿无效指针读写。
    for (unsigned i = 0; i < 8192; ++i) // 每一轮都检查复用页的全部字节。
        if (a[i]) // 旧所有者的填充值必须已经清零。
            return 122; // 不只检查第一个字节。
    memset(a, 0xa1, 8192); // 两页都留下可辨识内容。
    memset(b, 0xb2, 4096); // 独立邻居的保护字节。
    memset(c, 0xc3, 4096); // 后面的请求不能覆盖仍持有的页。
    if (airi_release(a + 1, 4096) != -22 || airi_release(a, 0) != -22 || airi_release(a, 1) != -22 || airi_release(a, SIZE_MAX) != -22) // 地址、长度与溢出输入均拒绝。
        return 123; // 无效请求不能被偷偷舍入。
    if (airi_release((void *)(uintptr_t)0x70000000, 4096) != -22) // 初始用户栈不是动态分配页。
        return 124; // 内核不能接受任意自有RW页的释放。
    if (airi_release(a + 4096, 16384) != -22 || a[4096] != 0xa1 || b[0] != 0xb2 || c[0] != 0xc3) // 范围末页未映射，前面三页也必须保持。
        return 125; // 验证整段先检查再提交。
    if (airi_release(a + 4096, 4096)) // 只释放a的第二页，第一页面向用户仍有效。
        return 126; // 页面接口允许完整页子范围。
    unsigned char *tail = airi_alloc(8192); // 单页洞容不下两页，须越过仍持有的b和c。
    if (!tail || (uintptr_t)tail != (uintptr_t)c + 4096 || c[0] != 0xc3) // 不把非连续物理页误当连续VA洞。
        return 127; // 活页被覆盖或选择范围错误。
    if (airi_release(b, 4096)) // 与刚才空洞连成两个连续用户虚拟页。
        return 128; // b之后不能再解引用。
    unsigned char *joined = airi_alloc(8192); // owned换序仍须找到低地址完整洞。
    if ((uintptr_t)joined != (uintptr_t)a + 4096) // 页面列表不是有序表，也能复用邻接洞。
        return 129; // 单向游标会在这里暴露。
    for (unsigned i = 0; i < 8192; ++i) // 这两页分别曾属于a和b。
        if (joined[i]) // 不允许保留任一旧填充值。
            return 130; // 页面归还与再次分配的清零都必须真实发生。
    if (airi_release(joined, 8192) || airi_release(tail, 8192)) // 归还两个新范围。
        return 131; // 不让本轮新对象留到进程exit。
    if (airi_release(a, 8192) != -22 || a[0] != 0xa1) // 第二页已经是洞，不能先释放第一页再报告失败。
        return 132; // 第一页仍须可访问。
    if (airi_release(a, 4096) || airi_release(c, 4096) || airi_release(a, 4096) != -22) // 清最后活页，并验证重复释放拒绝。
        return 133; // 下一轮仍从同一低地址空洞开始。
    return 0; // 当前进程还活着，页资源已在本轮归还。
}
int main(void) { // 测试的返回码由用户shell的wait报告。
    for (unsigned i = 0; i < 128; ++i) { // 累计分配远超每进程六十四页上限。
        int error = reuse_round(); // 多次页面创建与解除都在同一地址空间。
        if (error) // 第一次失败保留精确位置。
            return error; // 不重新启动规避资源泄漏。
    }
    volatile unsigned char *page = airi_alloc(4096); // 单独检查真实解除映射与fork所有权。
    if (!page) // 普通耗尽不能当作已准备好故障页。
        return 134; // shell收到明确错误。
    *page = 42; // 父子复制前的独立数据。
    long child = airi_fork(); // 子有同VA不同物理页，释放不应影响父。
    if (child < 0) // 不对失败身份执行wait。
        return 135; // 明确进程资源失败。
    if (!child) { // 故障只发生在拥有被解除页的孩子。
        if (airi_release((void *)page, 4096)) // 解除当前活动用户根的真实映射。
            return 136; // 失败不执行预期故障操作。
        return *page; // volatile强制真实读取，必须触发用户PF而不能命中旧TLB。
    }
    int status = 0; // 子用户故障由内核转成自有退出码。
    if (airi_waitpid(child, &status) != child || status != -142 || *page != 42) // 父地址空间与内容仍有效。
        return 137; // 错误解除共享页或未执行invlpg都会暴露。
    if (airi_release((void *)page, 4096)) // 父自己的页也正常归还。
        return 138; // 最后由launch再核对会话总资源。
    airi_puts("[USER_MEMORY_REUSE_OK] rounds=128 holes=first-fit zeroed=1 invalid=unchanged\n"); // 只在所有路径完成后发布。
    return 0; // shell仍活着，后续可继续运行orphans或其他程序。
}
