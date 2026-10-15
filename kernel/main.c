#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明


#ifndef AIRI_DEMO_MESSAGE  // 未由构建命令覆盖展示文字时使用下面默认值
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 默认日志输入，故障/修改 profile 可在编译时替换
#endif  // 结束当前条件编译范围，被排除的分支不会进入本次对象文件
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 接收适配层静态对象，成功后打印并停机，不返回
    const char *error = airi_boot_validate(info);  // 使用自有格式检查，先验证再遍历，成功为 NULL
    if (error)  // 错误指针非空表示验证失败，应显示其静态文字
        airi_panic(error);  // 沿统一串口通路报告首个验证错误并停机
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印格式身份与有效数量，字段类型和 %u 匹配
             info->loader);  // 显示自有名称数组，前面的验证已证明它有终止零
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 分别以十六进制起点和十进制字节跨度打印内核位置
             (unsigned long long)info->kernel_physical_base,  // 转换成格式 %llx 所读取的 unsigned long long，避免仅按位宽猜类型
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 虚拟起点和跨度也按 %llx/%llu 的实参类型转换
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 记录本次直接映射偏移，说明虚拟与物理描述的关系
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只检查本次有效地图项，不把未使用槽位算进去
        const struct airi_memory_region *r = &info->regions[i];  // 取当前条目地址供只读检查，不复制整段内存
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 每个有效条目输出一次，索引对应当前自有数组位置
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 把当前条目的物理起点和字节长度转为格式约定的类型
                 airi_memory_name(r->kind));  // 由自有枚举取得静态用途名称，不再访问外部协议常量
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 汇总引导分类的可用量，这里没有实时页分配扣减
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 将可用内存与仍保留的引导可回收内存分开报告
             (unsigned long long)usable,  // 把可用字节总量转成 %llu 对应的参数类型
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 统计保留中的引导范围，当前不把它并入可用量
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发出当前构建配置的展示文字，供完整重启观察
    airi_log("[AIRI_LESSON_READY] boot info validated; serial only");  // 必需启动结构已验证且日志遍历完成，最后发送成功标记
    airi_halt();  // 当前入口到此停止，不再向调用者返回
}
