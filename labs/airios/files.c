#include <airi_libc.h> // 真实用户程序通过int80操作FD，不调用内核函数。
int main(int argc, char **argv) { // 用户入口仍由自己的_start提供。
    if (argc != 1 || memcmp(argv[0], "files", 6)) // 验证装载的是这份ELF。
        return 40; // 不发布文件完成标记。
    char buffer[16] = {0}; // 同步读写缓冲在用户栈。
    if (airi_open("/missing", AIRI_READ) != -2 || airi_open("/seed", AIRI_WRITE) != -13) // 名称缺失和权限错误分开。
        return 41; // 文件层错误不能伪装成功。
    long fd = airi_open("/notes", AIRI_READ | AIRI_WRITE | AIRI_CREATE | AIRI_TRUNCATE); // 独立打开对象，内容在RAM。
    if (fd < 0 || airi_write((unsigned)fd, "abcdef", 6) != 6) // 只推进实际完成数量。
        return 42; // 后续偏移演示需要完整初值。
    if (airi_pwrite((unsigned)fd, "XY", 2, 1) != 2 || airi_pread((unsigned)fd, buffer, 8, 0) != 6 || memcmp(buffer, "aXYdef", 6)) // 显式定位覆盖与EOF短读。
        return 43; // 请求8只能返回实际6。
    if (airi_read((unsigned)fd, buffer, 1) != 0) // 前面的pread不能把原共享偏移6移回0。
        return 44; // EOF证明位置没有被pread移动。
    if (airi_seek((unsigned)fd, 1022) != 1022 || airi_write((unsigned)fd, "1234", 4) != 2 || airi_write((unsigned)fd, "x", 1) != -28) // 仅剩两字节产生短写，随后明确容量错误。
        return 45; // 不允许数组越界或假完成4。
    memset(buffer, 1, sizeof(buffer)); // 洞读取应覆盖为零。
    if (airi_pread((unsigned)fd, buffer, sizeof(buffer), 10) != 16) // 写到1022使10到1021成为零洞。
        return 46; // 长度应覆盖完整缓冲。
    for (unsigned i = 0; i < sizeof(buffer); ++i) // 不把初始化1误当文件内容。
        if (buffer[i]) // 每个洞字节都必须为零。
            return 47; // 防止truncate后的旧内容泄漏。
    if (airi_close((unsigned)fd) || airi_close((unsigned)fd) != -9) // 表项只能释放一次。
        return 48; // 内容仍在目录，打开对象已回收。
    fd = airi_open("/seed", AIRI_READ); // 槽0应被复用，供exec后的worker辨认本次FD演示。
    long duplicate = airi_dup((unsigned)fd, 1); // 成功exec关闭此表项，父亲的对应表项不受影响。
    if (fd != 0 || duplicate != 3 || airi_read((unsigned)fd, (void *)"readonly", 2) != -14) // 只读用户输出缓冲必须在消费偏移前拒绝。
        return 49; // 下一次读仍应从a开始。
    long pid = airi_fork(); // 子表项引用相同打开对象，用户缓冲页独立复制。
    if (pid < 0) // 容量问题显式处理。
        return 50; // 不继续等待不存在孩子。
    if (!pid) { // 孩子先读ab，再exec让新程序读cd。
        if (airi_read((unsigned)fd, buffer, 2) != 2 || memcmp(buffer, "ab", 2)) // 真正消费共享偏移。
            return 51; // 父亲会收到失败状态。
        airi_exec("worker"); // 普通FD保留，CLOEXEC关闭，PID仍是本孩子。
        return 52; // 成功不能返回原映像。
    }
    int status = -1; // 自有wait返回普通退出值。
    if (airi_waitpid(pid, &status) != pid || status != 23) // worker完成继承检查后返回23。
        return 53; // 不能只观察孩子创建成功。
    if (airi_read((unsigned)duplicate, buffer, 2) != 2 || memcmp(buffer, "ef", 2)) // 子exec后读cd已前移同一打开对象，父dup仍开放。
        return 54; // fork+exec的共享偏移真实生效。
    if (airi_close((unsigned)fd) || airi_read((unsigned)duplicate, buffer, 1) != 0 || airi_close((unsigned)duplicate)) // 最后引用才回收，不因父关闭一个编号破坏另一个。
        return 55; // 必须正确到达EOF。
    airi_puts("[FILES_USER_OK] short_io=1 holes=zero pread_offset=stable fork_offset=shared cloexec=per_fd\n"); // 仅所有实际检查成立后发布。
    return 0; // launch随后核对内存和全局打开对象恢复。
}
