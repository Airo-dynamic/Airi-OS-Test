#include <airi_libc.h> // 原始页申请用于构造跨页坏描述，文件调用走用户包装。
#include <errno.h> // 向量错误同样返回-1和当前本地正错误号。
#include <fcntl.h> // 平坦RAM文件与访问模式。
#include <unistd.h> // 普通读回与共享偏移观察。
#include <sys/uio.h> // 本节实际提供preadv/pwritev，不宣称readv/writev已支持阻塞流。
int main(void) { // 所有缓冲区都在真实用户页内，未由宿主注入数据。
    int fd = open("/vector", O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600); // 新打开对象偏移0。
    if (fd < 0) // 保留普通错误结果。
        return 1; // 不进行后续比较。
    struct iovec input[3] = {{"ab", 2}, {"CDE", 3}, {"f", 1}}; // 分散用户地址按数组顺序聚合成abCDEf。
    if (pwritev(fd, input, 3, 0) != 6) // 一次显式偏移调用，不是三次文件写。
        return 2; // 不接受只处理第一段的伪向量。
    unsigned char a[2], b[4], c[1]; // EOF将落在第二个目标内部。
    memset(a, '?', sizeof(a)); memset(b, '?', sizeof(b)); c[0] = '?'; // 尚未读到的尾部必须保持。
    struct iovec output[3] = {{a, sizeof(a)}, {b, sizeof(b)}, {c, sizeof(c)}}; // 总容量7，offset1之后只有5字节。
    if (preadv(fd, output, 3, 1) != 5 || memcmp(a, "bC", 2) || memcmp(b, "DEf", 3) || b[3] != '?' || c[0] != '?') // 按顺序填满a，只填b前三字节，不碰尾部。
        return 3; // 短返回总数不能变成每段都填满。
    if (read(fd, c, 1) != 1 || c[0] != 'a') // 前面的显式读写都不能改变原offset0。
        return 4; // preadv不是先seek再read。
    if (pwritev(fd, input, 2, 1022) != 2) // RAM上限1024导致短写恰好结束在第一项。
        return 5; // 不将文件容量限制伪装为全部完成。
    if (preadv(fd, output, 3, 1022) != 2 || memcmp(a, "ab", 2)) // 实际读回最后两字节。
        return 6; // 不能仅靠返回2作证。
    unsigned char first[200], second[200], data[300]; // 大于一次内核前缀预算的逻辑向量。
    memset(first, 0x31, sizeof(first)); memset(second, 0x79, sizeof(second)); memset(data, '?', sizeof(data)); // 两个来源可独立辨识。
    struct iovec large[2] = {{first, sizeof(first)}, {second, sizeof(second)}}; // 总400，当前一次最多256。
    if (pwritev(fd, large, 2, 0) != 256) // 第一项200加第二项56，返回前缀交调用者继续。
        return 7; // 不是每项各允许256。
    struct iovec scatter[2] = {{data, 100}, {data + 100, 200}}; // 总300，只分散实际256。
    if (preadv(fd, scatter, 2, 0) != 256) // 一次读取的连续文件范围。
        return 8; // 不隐式循环读取剩下44字节。
    for (unsigned i = 0; i < 300; ++i) // 比较完整输出，含未触及尾部。
        if (data[i] != (i < 200 ? 0x31 : i < 256 ? 0x79 : '?')) // 200之后进入第二个来源，256以后保持哨兵。
            return 9; // 不让scatter覆盖额外字节。
    struct iovec bad[2] = {{"Z", 1}, {(void *)(uintptr_t)0xffff800000000000ull, 1}}; // 第一项合法，第二项在本次实际前缀内非法。
    if (pwritev(fd, bad, 2, 0) != -1 || errno != EFAULT || pread(fd, c, 1, 0) != 1 || c[0] != 0x31) // 写前完整检查，不能留下第一项Z。
        return 10; // 逐项发普通write会在这里暴露半次副作用。
    c[0] = '?'; bad[0] = (struct iovec){c, 1}; // 改为读，第一项是有效输出。
    if (preadv(fd, bad, 2, 0) != -1 || errno != EFAULT || c[0] != '?') // 坏后项不能让前一个输出先被消费填入。
        return 11; // 原哨兵必须保持。
    if (preadv(fd, (void *)(uintptr_t)0xffff800000000000ull, 1, 0) != -1 || errno != EFAULT) // 坏描述数组由内核检查，用户库不得先解引用崩溃。
        return 12; // 必须仍在此用户中继续运行。
    if (pwritev(fd, NULL, -1, 0) != -1 || errno != EINVAL || pwritev(fd, NULL, IOV_MAX + 1, 0) != -1 || errno != EINVAL) // 负数量与超过八项都在读描述前拒绝。
        return 13; // 不因NULL指针掩盖数量校验。
    struct iovec overflow[2] = {{c, (size_t)INT64_MAX}, {c, 1}}; // 两项总量超出ssize_t，即使前256原本可能可读也不能接受。
    if (pwritev(fd, overflow, 2, 0) != -1 || errno != EINVAL || pwritev(fd, input, 1, -1) != -1 || errno != EINVAL) // 长度溢出与负显式偏移都无副作用。
        return 14; // 不把回绕后的短值当合法请求。
    struct iovec empty[2] = {{(void *)(uintptr_t)UINT64_MAX, 0}, {"ok", 2}}; // 零长项没有可访问字节，基址不参与本次检查。
    if (pwritev(fd, empty, 2, 10) != 2 || pwritev(fd, NULL, 0, 0) != 0 || preadv(-1, NULL, 0, 0) != -1 || errno != EBADF) // 空向量不读取描述，但仍验证FD。
        return 15; // 不把全部零长度误当任意FD都成功。
    unsigned char *page = airi_alloc(4096); // 下一页未映射，构造真实跨页描述而非宿主数组。
    if (!page) // 明确处理资源不足。
        return 16; // 不访问NULL。
    *(uint64_t *)(page + 4088) = (uintptr_t)c; // 描述只有第一个字段可读，长度字段落到下一页。
    if (preadv(fd, (const struct iovec *)(page + 4088), 1, 0) != -1 || errno != EFAULT) // 整个16字节数组必须在操作前复制。
        return 17; // 不将有效半项当完整描述。
    struct iovec crosses = {page + 4094, 4}; // 数据缓冲前两字节合法，后两字节跨未映射边界。
    page[4094] = 0x55; page[4095] = 0x55; // 验证坏范围不改有效前缀。
    if (preadv(fd, &crosses, 1, 0) != -1 || errno != EFAULT || page[4094] != 0x55 || page[4095] != 0x55 || airi_release(page, 4096)) // 数据跨度完整校验后才可读文件。
        return 18; // 不是只检查指针起点。
    int ends[2]; // 当前显式偏移向量不进入阻塞流等待。
    if (pipe(ends) || pwritev(ends[1], input, 1, 0) != -1 || errno != ESPIPE || close(ends[0]) || close(ends[1]) || close(fd)) // 一次文件层操作保留流的不可定位语义。
        return 19; // 不对管道伪造偏移。
    airi_puts("[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked\n"); // 所有正常与故障路径实际完成。
    return 0; // 用户shell继续服务，整次会话核对资源恢复。
}
