#include <airi/kernel.h>   // 使用启动描述和当前可选显示接口。
#include <airi/console.h>  // 接受同一文本会话；本目标不持有图形状态。

bool airi_console_draw(struct airi_console *console) {  // 串口目标没有像素消费者。
    (void)console;  // 明确本实现无需访问会话；不解引用可选帧缓冲。
    return false;   // 返回未绘图，使入口继续保留串口输入输出。
}
bool airi_dashboard_draw(const AiriBootInfo *info) {  // 与可选图形实现保持相同链接接口。
    (void)info;    // 串口运行不需要设备像素地址。
    return false;  // 不以设备存在冒充完成了一帧。
}
