#include <airi_libc.h> // 真正ring3管道工作负载。
int main(void) { // 本节全部显式非阻塞，暂不让内核睡眠。
    unsigned ends[2]; // 由内核原子填写读/写FD。
    if (airi_pipe(ends, 1)) // 1表示本节已实现的非阻塞模式。
        return 70; // 创建失败不能用未初始化编号。
    char data[100], output[100]; // 两个用户缓冲，设备不持有其地址。
    for (unsigned i = 0; i < sizeof(data); ++i) // 使用不同字节检验FIFO和回绕。
        data[i] = (char)i; // 可复现序列0–99。
    if (airi_ready(ends[0]) != 0 || airi_ready(ends[1]) != AIRI_WRITABLE || airi_read(ends[0], output, 1) != -11) // 空流是暂不可读，不是EOF。
        return 71; // 保留写端时read不能提前返回0。
    if (airi_write(ends[1], data, 100) != 64 || airi_write(ends[1], data, 1) != -11 || airi_ready(ends[1]) != 0) // 固定64字节容量产生真实短写和背压。
        return 72; // 不把满环当已完成100字节。
    if (airi_read(ends[0], output, 10) != 10 || memcmp(output, data, 10) || airi_write(ends[1], data, 10) != 10) // 先释放十个空位，再回绕写十字节。
        return 73; // 只写当前有空间的前缀。
    if (airi_read(ends[0], output, 100) != 64 || memcmp(output, data + 10, 54) || memcmp(output + 54, data, 10)) // FIFO次序必须跨环尾保持。
        return 74; // 未读数据不能被覆盖。
    if (airi_close(ends[1]) || airi_ready(ends[0]) != (AIRI_READABLE | AIRI_HANGUP) || airi_read(ends[0], output, 1) != 0 || airi_close(ends[0])) // 最后写者关闭才变EOF。
        return 75; // HUP和可读EOF可以同时存在。
    if (airi_pipe(ends, 1) || airi_close(ends[0]) || airi_write(ends[1], data, 1) != -32 || airi_ready(ends[1]) != AIRI_ERROR || airi_close(ends[1])) // 无读者写为EPIPE，不发Linux SIGPIPE。
        return 76; // 错误也是可查询状态。
    if (airi_pipe(ends, 1)) // 新管道必须不含上次字节。
        return 77; // 不能复用仍被持有的环。
    long child = airi_fork(); // 父子复制表项，端点打开对象的引用增加。
    if (child < 0) // 明确进程容量错误。
        return 78; // 不等待不存在孩子。
    if (!child) { // 孩子保留写端，父保留读端。
        if (airi_close(ends[0]) || airi_write(ends[1], "child", 5) != 5 || airi_close(ends[1])) // 真正用户间传字节。
            return 79; // 父wait取得具体失败。
        return 0; // exit清余下FD引用。
    }
    int status = -1; // 自有普通退出码。
    if (airi_close(ends[1]) || airi_waitpid(child, &status) != child || status || airi_read(ends[0], output, 10) != 5 || memcmp(output, "child", 5) || airi_read(ends[0], output, 1) != 0 || airi_close(ends[0])) // 父关闭自己写端，孩子最后写端也关闭后才能EOF。
        return 80; // 不能因一个FD关闭就提前消灭整个管道。
    airi_puts("[PIPE_USER_OK] short_write=64 backpressure=-11 wrap=fifo eof=0 broken=-32 fork=shared\n"); // 所有真实用户状态成立后发布。
    return 0; // 整个会话还会核对全局对象回收。
}
