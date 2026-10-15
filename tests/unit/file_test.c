#include <airi/file.h> // 与guest相同的文件算法，宿主不访问UART。
#include <assert.h> // 每条断言验证具体所有权或边界。
#include <stdio.h> // 宿主只输出最终结果。
int main(void) { // 独立进程，每次建立干净RAM目录。
    airi_files_init(); // 与启动一致的只读seed。
    struct airi_fd_table parent, child; // 两份进程表引用同一全局对象。
    airi_fd_init(&parent); // stdout/stderr占1和2，槽0可用。
    assert(airi_fd_open(&parent, "/seed", AIRI_OPEN_WRITE) == -13); // 只读节点不能绕过。
    assert(airi_fd_open(&parent, "/bad/path", AIRI_OPEN_READ) == -22); // 不假装有目录遍历。
    long fd = airi_fd_open(&parent, "/seed", AIRI_OPEN_READ); // 第一份独立打开对象。
    assert(fd == 0); // 最小空编号。
    long dupped = airi_fd_dup(&parent, (unsigned)fd, true); // 同一对象的第二表项。
    assert(dupped == 3 && airi_files_open_count() == 1); // dup不新建全局对象。
    airi_fd_clone(&child, &parent); // fork复制表项、增加引用。
    char data[16] = {0}; // 核对实际返回的文件字节。
    assert(airi_fd_read(&child, 0, data, 2, false, 0) == 2 && !airi_memcmp(data, "ab", 2)); // 子先消费ab。
    airi_fd_close_all(&child, true); // 成功exec只关其CLOEXEC项。
    assert(airi_fd_read(&child, 3, data, 1, false, 0) == -9); // 关闭不影响父同号。
    assert(airi_fd_read(&parent, 3, data, 2, false, 0) == 2 && !airi_memcmp(data, "cd", 2)); // 父dup看到共享偏移。
    assert(airi_fd_read(&child, 0, data, 8, true, 0) == 6); // pread读6字节，不改变偏移4。
    assert(airi_fd_read(&child, 0, data, 2, false, 0) == 2 && !airi_memcmp(data, "ef", 2)); // 普通读继续4。
    airi_fd_close_all(&child, false); // 子退出释放其余引用。
    airi_fd_close_all(&parent, false); // 最后引用归还全局对象。
    assert(!airi_files_open_count()); // 不遗留只增不减的引用。
    airi_fd_init(&parent); // 新会话不清除文件内容。
    fd = airi_fd_open(&parent, "/notes", 15); // 读写+创建+清空。
    assert(fd == 0); // 编号复用。
    assert(airi_fd_write(&parent, 0, "abcdef", 6, false, 0) == 6); // 初始内容。
    assert(airi_fd_write(&parent, 0, "X", 1, true, 1023) == 1); // 显式定位使中间成为洞。
    assert(airi_fd_write(&parent, 0, "Z", 1, true, 1024) == -28); // 容量满错误。
    assert(airi_fd_read(&parent, 0, data, 16, true, 10) == 16); // 洞可见为零。
    for (unsigned i = 0; i < sizeof(data); ++i) // 所有字节必须被明确初始化。
        assert(data[i] == 0); // 不泄漏旧内容。
    while (airi_fd_dup(&parent, 0, false) >= 0) {} // 填满进程表，多个表项仍只用一个对象。
    assert(airi_fd_open(&parent, "/notes", 11) == -24); // 表满时TRUNCATE必须尚未执行。
    assert(airi_fd_read(&parent, 0, data, 6, true, 0) == 6 && !airi_memcmp(data, "abcdef", 6)); // 失败open未清旧内容。
    airi_fd_close_all(&parent, false); // 释放所有共享引用。
    assert(!airi_files_open_count()); // 同一对象不会被过早或重复回收。
    struct airi_fd_table tables[2]; // 两个进程可耗尽全局24个对象。
    airi_fd_init(&tables[0]); // 每个进程还有十四个空FD。
    airi_fd_init(&tables[1]); // 全局容量与单进程上限不同。
    for (unsigned i = 0; i < 24; ++i) // 第一表开14份，第二表开10份。
        assert(airi_fd_open(&tables[i < 14 ? 0 : 1], "/seed", AIRI_OPEN_READ) >= 0); // 每次open得到独立对象。
    assert(airi_fd_open(&tables[1], "/notes", 11) == -23); // 还有FD但没有打开对象，不能truncate。
    airi_fd_close_all(&tables[0], false); // 清全局引用的一部分。
    airi_fd_close_all(&tables[1], false); // 清剩余部分。
    assert(!airi_files_open_count()); // 所有槽均可再次复用。
    airi_fd_init(&parent); // 继续用同一文件系统测试管道资源，不重置已有状态。
    unsigned ends[2] = {77, 88}; // 创建失败必须保留输出身份。
    assert(!airi_fd_pipe(&parent, ends, true) && ends[0] == 0 && ends[1] == 3); // 两个最低空FD。
    assert(airi_fd_ready(&parent, 0) == 0 && airi_fd_ready(&parent, 3) == AIRI_READY_WRITE); // 空管道的双端条件不同。
    assert(airi_fd_read(&parent, 0, data, 1, false, 0) == -11); // 有写者时空读不是EOF。
    char input[100], output[100]; // 模式覆盖整个环并越过容量。
    for (unsigned i = 0; i < sizeof(input); ++i) // 每个位置有明确值。
        input[i] = (char)i; // 可检查FIFO和回绕。
    assert(airi_fd_write(&parent, 3, input, 100, false, 0) == 64); // 容量边界产生短写。
    assert(airi_fd_write(&parent, 3, input, 1, false, 0) == -11 && airi_fd_ready(&parent, 3) == 0); // 满流形成背压。
    assert(airi_fd_read(&parent, 0, output, 10, false, 0) == 10 && !airi_memcmp(output, input, 10)); // 先释放一段空间。
    assert(airi_fd_write(&parent, 3, input, 10, false, 0) == 10); // 新数据跨环尾。
    assert(airi_fd_read(&parent, 0, output, 100, false, 0) == 64 && !airi_memcmp(output, input + 10, 54) && !airi_memcmp(output + 54, input, 10)); // 完整FIFO次序不被回绕改变。
    airi_fd_clone(&child, &parent); // fork后端点存在直到最后共享引用消失。
    assert(!airi_fd_close(&parent, 3) && airi_fd_read(&parent, 0, data, 1, false, 0) == -11); // 子仍持写端，不能提前EOF。
    assert(!airi_fd_close(&child, 3) && airi_fd_ready(&parent, 0) == (AIRI_READY_READ | AIRI_READY_HUP)); // 最后写引用消失才HUP。
    assert(airi_fd_read(&parent, 0, data, 1, false, 0) == 0); // 排空后真正EOF。
    airi_fd_close_all(&child, false); // 关闭子读端引用，不破坏父。
    airi_fd_close_all(&parent, false); // 最后读端结束，缓冲可复用。
    assert(!airi_files_open_count()); // 两端对象全部归还。
    airi_fd_init(&parent); // 新会话再建新管道。
    assert(!airi_fd_pipe(&parent, ends, true)); // 旧流字节必须清零丢弃。
    assert(!airi_fd_close(&parent, ends[0])); // 不再存在任何读者。
    assert(airi_fd_write(&parent, ends[1], input, 1, false, 0) == -32 && airi_fd_ready(&parent, ends[1]) == AIRI_READY_ERROR); // 写端收到EPIPE和错误就绪。
    assert(airi_fd_write(&parent, ends[1], NULL, 0, false, 0) == 0); // 零长度不访问buffer，也不等待对端。
    airi_fd_close_all(&parent, false); // 回收断流。
    airi_fd_init(&parent); // 四个流总共八个端点，小于本进程FD容量。
    for (unsigned i = 0; i < 4; ++i) // 填满独立管道池。
        assert(!airi_fd_pipe(&parent, ends, true)); // 每次两个端点完整成功。
    ends[0] = 77; ends[1] = 88; // 失败输出哨兵。
    assert(airi_fd_pipe(&parent, ends, true) == -28 && ends[0] == 77 && ends[1] == 88 && airi_files_open_count() == 8); // 第五条失败不泄漏半个FD或对象。
    airi_fd_close_all(&parent, false); // 扫描全部表项，端点池也全部回收。
    assert(!airi_files_open_count()); // 后续程序可继续创建流。
    airi_fd_init(&parent); airi_fd_attach_input(&parent); // 只给新shell根接入固定输入。
    assert(airi_fd_input(&parent,0) && parent.entries[0].cloexec); // FD0与输出对象身份不同，exec时必须撤销。
    airi_fd_clone(&child,&parent); // fork增加输入对象引用，但不复制设备。
    airi_fd_close_all(&child,true); // 成功exec只关闭CLOEXEC输入，保留stdout/stderr。
    assert(!airi_fd_input(&child,0) && airi_fd_input(&parent,0)); // 子撤销不影响父shell继续读。
    airi_fd_close_all(&child,false); airi_fd_close_all(&parent,false); // 最后输入引用释放不能清空固定对象配置。
    airi_fd_init(&parent); airi_fd_attach_input(&parent); // 再次启动shell可重新使用相同输入对象。
    assert(airi_fd_input(&parent,0)); airi_fd_close_all(&parent,false); // 重复会话完整回收。
    puts("PASS RAM files/pipes: offsets, fork/exec references, sparse zero, short I/O, FIFO wrap, endpoint lifetime, exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
