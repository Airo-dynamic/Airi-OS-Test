#include <airi/console.h>  // 引入命令状态和逻辑键接口
#include <stdio.h>  // 主机用puts和printf观察同一控制台的输出
static struct airi_console console;     // 静态存储拥有输入、历史与输出
static void submit(const char *text) {  // 借用一条带换行的NUL命令，只在调用期间读取
    while (*text)  // 遇终止零结束，字符串中的换行仍作为提交事件送入
        airi_console_key(&console, *text++);  // 先取当前字节，调用命令入口，再把指针推进一字节
}
int main(void) {  // 用真实命令调用观察查询、回显与未知命令
    AiriBootInfo boot = {.version = 1, .region_count = 1};  // 当前命令观察只需要一条可用区域，不调用完整引导验证
    boot.regions[0] = (struct airi_memory_region){0x100000, 0x400000, AIRI_MEMORY_USABLE, 0};  // 物理1MiB起提供4MiB可用范围
    airi_console_init(&console, &boot);  // 初始化输出与历史，并借用当前启动结构
    submit("mem\n");  // 当前构造输入提供4096KiB可用内存。
    submit("echo serial-mainline\n");  // 文本参数经同一命令路径回显。
    submit("theme day\n");  // 未安装图形消费者时报告未知命令。
    submit("devices\n");    // 查询固定设备端点。
    submit("help\n");  // 列出当前已实现命令。
    submit("sys\n");   // 查询相同启动结构的身份。
    for (uint64_t i = 0; i < console.total_lines; ++i)  // 沿有效逻辑行号读取本次全部实际输出
        puts(airi_console_line(&console, i));  // 输出环中的每条NUL文字显示在主机终端
    printf("commands=%u\n", console.commands);  // 报告同一会话的累计提交数
    return console.commands != 6;  // 六次提交全部处理后退出0
}
