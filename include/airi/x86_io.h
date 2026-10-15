#ifndef AIRI_X86_IO_H // 端口与IF操作仅供x86平台和设备适配。
#define AIRI_X86_IO_H // 包含保护。
#include <stdint.h> // 硬件字段宽度。
static inline uint8_t airi_in8(uint16_t port) { // 从一个I/O端口读取八位。
    uint8_t value; // AL接收设备值。
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port)); // 常量端口或DX，不能优化掉真实访问。
    return value; // 值不是内存地址。
}
static inline uint16_t airi_in16(uint16_t port) { // 队列大小等十六位字段。
    uint16_t value; // AX接收。
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port)); // 读取设备端口。
    return value; // 保留字段宽度。
}
static inline uint32_t airi_in32(uint16_t port) { // PCI和VirtIO三十二位字段。
    uint32_t value; // EAX接收。
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port)); // 真实端口访问。
    return value; // 不做宿主系统调用。
}
static inline void airi_out8(uint16_t port, uint8_t value) { // 写设备状态等八位字段。
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory"); // memory阻止普通内存发布越过设备通知。
}
static inline void airi_out16(uint16_t port, uint16_t value) { // 写队列号与通知。
    __asm__ volatile("outw %0, %1" :: "a"(value), "Nd"(port) : "memory"); // AX写DX指定端口。
}
static inline void airi_out32(uint16_t port, uint32_t value) { // 写配置与队列PFN。
    __asm__ volatile("outl %0, %1" :: "a"(value), "Nd"(port) : "memory"); // EAX提供三十二位值。
}
static inline uint64_t airi_irq_save(void) { // 暂停本CPU外部可屏蔽IRQ，调用者必须恢复原IF。
    uint64_t flags; // 保存调用前RFLAGS。
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory"); // 保存再关，不把不可抢占内核变成多核锁。
    return flags; // 只用IF决定恢复方式。
}
static inline void airi_irq_restore(uint64_t flags) { // 不修改其他特权标志。
    if (flags & 0x200) // 原调用者允许中断。
        __asm__ volatile("sti" ::: "memory"); // 恢复IF，不无条件打开系统调用上下文。
}
#endif // 结束保护。
