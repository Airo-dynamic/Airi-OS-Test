#ifndef AIRI_BLOCK_H // 通用块接口不暴露PCI端口和VirtIO布局。
#define AIRI_BLOCK_H // 包含保护。
#include <airi/boot.h> // 当前启动提供内核连续物理映像位置。
int airi_block_init(const AiriBootInfo *info); // 固定单队列，失败返回负错误。
bool airi_block_interrupt(void); // IRQ只读清ISR并记录提示，不收集DMA结果。
uint64_t airi_block_interrupts(void); // 实际设备IRQ计数，不等于完成条数。
bool airi_block_readonly(void); // 给调用者明确只读事实，不能靠写失败猜测。
uint64_t airi_block_sectors(void); // 以512字节扇区计，不等同设备逻辑块大小配置。
int airi_block_read(uint64_t sector, void *buffer); // 一次精确一个512字节扇区，0成功。
int airi_block_write(uint64_t sector, const void *buffer); // 同步复制并等待设备完成。
int airi_block_flush(void); // 请求后端同步，不宣称真实硬件掉电已实测。
int airi_block_begin(unsigned operation, uint64_t sector, const void *write_data); // 0读/1写/4同步；0表示已接收，不表示完成。
int airi_block_poll(void *read_data); // 1仍在途、0完成、负错误；读输出可NULL表示丢弃，不释放DMA前不重用。
bool airi_block_pending(void); // 已接收但未收集完成，含设备已完成却未poll的状态。
void airi_block_demo(void); // 只操作项目独立实验盘末扇区。
#endif // 结束保护。
