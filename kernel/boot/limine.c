#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明
#include <limine.h>  // 读取锁定协议的类型与标识常量，头文件本身不执行引导
__attribute__((used, section(".limine_requests_start")))  // 要求保留下一对象并放进扫描起始标记节
static volatile uint64_t start[] = LIMINE_REQUESTS_START_MARKER;  // 协议规定的起始字节数组，构建时不可丢弃
__attribute__((used, section(".limine_requests")))  // 将下一对象放入引导器扫描的请求节
static volatile uint64_t revision[] = LIMINE_BASE_REVISION(6);  // 请求基础协议修订 6，Limine 会在交接前更新支持状态
__attribute__((used, section(".limine_requests_end")))  // 将下一对象放到扫描范围末尾标记节
static volatile uint64_t end[] = LIMINE_REQUESTS_END_MARKER;  // 结束标记界定本次请求记录范围
_Noreturn void airi_limine_start(void) {  // 汇编 call 进入的 C 入口，当前栈已满足调用约定
    airi_serial_init();  // 先配置串口，后续协议错误也能通过同一设备报告
    airi_serial_write("\n");  /* End any firmware terminal-control line. */  // 先结束固件可能残留的控制序列所在行，再输出内核标记
    if (!LIMINE_BASE_REVISION_SUPPORTED(revision))  // 读取引导器写回的支持状态，修订不符则停止
        airi_panic("Limine revision 6 is unsupported");  // 具体报告所需修订，错误路径不继续发送成功标记
    airi_log("[AIRI_LESSON_READY] _start -> C -> UART");  // 当前栈与设备路径都已走通，发出观察器期待的完整日志
    airi_halt();  // 当前入口到此停止，不再向调用者返回
}
