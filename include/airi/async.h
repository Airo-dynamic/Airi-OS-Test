#ifndef AIRI_ASYNC_H // 有界异步算法与进程、PCI适配分开。
#define AIRI_ASYNC_H // 包含保护。
#include <airi/base.h> // 复制、长度和固定宽度。
#include <airi/io_abi.h> // 自有提交/完成数值记录。
#include <stdbool.h> // 引擎状态。
#define AIRI_AIO_OWNERS 8u // 与当前进程预算相同，算法本身只认识owner编号。
#define AIRI_AIO_BUFFERS 32u // 每owner最多四个注册缓冲。
#define AIRI_AIO_REQUESTS 4u // 全局排队与在途合计四个。
#define AIRI_AIO_CQ 4u // 每owner最多四个未收集完成，提交先预留信用。
enum airi_aio_buffer_state { AIRI_AIO_BUFFER_FREE, AIRI_AIO_BUFFER_IDLE, AIRI_AIO_BUFFER_BUSY }; // BUSY直到CQ被收集。
enum airi_aio_request_state { AIRI_AIO_REQUEST_FREE, AIRI_AIO_QUEUED, AIRI_AIO_ACTIVE }; // ACTIVE不能在owner退出时提前重用。
#define AIRI_AIO_SUBMIT 1u // SQ接受后，尚未进入设备。
#define AIRI_AIO_START 2u // 后端begin实际接收。
#define AIRI_AIO_COMPLETE 3u // 当前owner的CQ形成。
#define AIRI_AIO_DROP 4u // 无主或旧epoch完成被丢弃。
#define AIRI_AIO_CANCEL 5u // owner关闭时撤销尚未开始的项。
struct airi_aio_request; // 观察回调只在当前调用内借请求元数据。
struct airi_aio_backend { // 可信单消费者后端，begin不等待、poll只检查一次。
    void *context; // 后端自有状态寿命覆盖引擎。
    int (*begin)(void *, unsigned, uint64_t, const void *); // 0接收，-16忙，其他负错误形成CQ。
    int (*poll)(void *, void *); // 1在途、0完成、负错误。
    void (*event)(void *, unsigned, const struct airi_aio_request *, long); // 可选同步观察，只读且不得重入或修改引擎。
};
struct airi_aio_owner { // 每次打开/重置拥有新的epoch。
    unsigned id, outstanding, head, count; // 身份、预留信用和CQ环位置。
    uint64_t epoch; // 同一PID exec后仍能区分旧映像请求。
    struct airi_io_completion completions[AIRI_AIO_CQ]; // 有界结果，不借用户内存。
};
struct airi_aio_buffer { // 设备不直接DMA到这里，队列单元另持有请求副本。
    unsigned owner; // ID0保留为无owner。
    uint64_t epoch, handle; // owner轮次与代数句柄同时校验。
    enum airi_aio_buffer_state state; // free/idle/busy决定修改与释放权限。
    uint8_t data[512]; // 固定注册缓冲，重用先清零。
};
struct airi_aio_request { // 即使owner退出，ACTIVE单元仍由引擎持有。
    enum airi_aio_request_state state; // 队列槽的真实生命周期。
    unsigned owner, operation; // 不借进程控制块指针。
    uint64_t epoch, handle, cookie, sequence, sector; // 身份与FIFO提交顺序分别保存。
    uint8_t data[512]; // 写提交时复制，读完成时暂存；不借注册缓冲地址。
};
struct airi_aio_engine { // 固定静态对象，不能复制后当两份资源管理器。
    struct airi_aio_backend backend; // 单设备接收/完成接口。
    struct airi_aio_owner owners[AIRI_AIO_OWNERS]; // owner生命周期独立于表槽复用。
    struct airi_aio_buffer buffers[AIRI_AIO_BUFFERS]; // 全局注册池。
    struct airi_aio_request requests[AIRI_AIO_REQUESTS]; // SQ与单在途共享容量。
    int active; // -1无硬件在途，否则指向ACTIVE槽。
    uint64_t epoch, generation, sequence, base, sectors; // 代数不倒退，用户块范围与正式日志隔离。
    bool faulted; // 内部不变量失配后不能继续静默运行。
};
int airi_aio_init(struct airi_aio_engine *engine, const struct airi_aio_backend *backend, uint64_t base, uint64_t sectors); // 初始化没有借用者的新引擎。
int airi_aio_open(struct airi_aio_engine *engine, unsigned owner); // 新owner编号不能已有上下文。
int airi_aio_reset(struct airi_aio_engine *engine, unsigned owner); // 检查新epoch后撤销旧轮次，失败不改变旧状态。
void airi_aio_close(struct airi_aio_engine *engine, unsigned owner); // 撤销排队和CQ，ACTIVE保留到真实收集。
long airi_aio_buffer_alloc(struct airi_aio_engine *engine, unsigned owner); // 返回正代数句柄，失败负数。
long airi_aio_buffer_put(struct airi_aio_engine *engine, unsigned owner, uint64_t handle, const void *data, size_t bytes); // idle才可修改，尾部清零。
long airi_aio_buffer_get(struct airi_aio_engine *engine, unsigned owner, uint64_t handle, void *data, size_t bytes); // idle才可读，错误不会复制。
long airi_aio_buffer_free(struct airi_aio_engine *engine, unsigned owner, uint64_t handle); // busy拒绝释放。
int airi_aio_submit(struct airi_aio_engine *engine, unsigned owner, unsigned operation, uint64_t handle, uint64_t sector, uint64_t cookie); // 只排队与预留CQ信用，0不表示完成。
int airi_aio_pop(struct airi_aio_engine *engine, unsigned owner, struct airi_io_completion *completion); // 空返回-11，收集后才解锁缓冲。
int airi_aio_pump(struct airi_aio_engine *engine); // 有界一次服务，不睡眠；内部不变量错误-5。
bool airi_aio_work(const struct airi_aio_engine *engine); // 包含无owner的ACTIVE，不含仅等待用户收集的CQ。
int airi_aio_state(struct airi_aio_engine *engine, unsigned owner, struct airi_io_state *state); // 当前owner轮次的只读数值。
bool airi_aio_quiescent(const struct airi_aio_engine *engine); // 所有owner、buffer和在途单元均已释放。
void airi_async_init(void); // 所有设备配置都初始化资源管理器，后端错误通过CQ报告。
int airi_async_open(unsigned pid); // 新进程空上下文，不继承父注册buffer。
int airi_async_reset(unsigned pid); // exec提交前换epoch。
void airi_async_close(unsigned pid); // 退出撤销owner，ACTIVE留内核收集。
long airi_async_buffer_alloc(unsigned pid); // 当前PID自己的代数句柄。
long airi_async_buffer_put(unsigned pid, uint64_t handle, const void *data, size_t bytes); // 内核副本输入。
long airi_async_buffer_get(unsigned pid, uint64_t handle, void *data, size_t bytes); // 内核副本输出。
long airi_async_buffer_free(unsigned pid, uint64_t handle); // BUSY拒绝。
int airi_async_submit(unsigned pid, unsigned operation, uint64_t handle, const struct airi_io_submission *submission); // 已复制的提交数值。
int airi_async_pop(unsigned pid, struct airi_io_completion *completion); // 空CQ为-11。
int airi_async_state(unsigned pid, struct airi_io_state *state); // 提供给用户或未来显示消费者的数值快照。
bool airi_async_work(void); // 包含无主ACTIVE。
void airi_async_pump(void); // 只在长期内核服务栈推进。
void airi_async_drain(void); // 最后用户退出后结束内核持有的ACTIVE，并核对完全回收。
void airi_async_demo(void); // 固定QEMU内核队列演示，用户API接下一检查点。
#endif // 结束保护。
