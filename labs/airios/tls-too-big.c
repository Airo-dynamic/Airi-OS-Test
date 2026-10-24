#include <airi_libc.h> // 这份ELF只用于装载边界，正常实现不能进入main。
_Thread_local volatile unsigned char excess[4097]; // 比本轮模板上限4096多一个字节。
int main(void) { // 若装载器忽略PT_TLS，不能以无输出返回冒充拒绝。
    excess[0] = excess[4096]; // 两个端点都被真实使用，编译器不能删掉超限对象。
    return 222; // 用户工作负载应收到exec的-8，而不是此退出码。
}
