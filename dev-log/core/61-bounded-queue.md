# K61 · 有界工作队列、背压与关闭排空

K60 的版本只回答“是否需要重查”。现在把要重查的条件变成真实工作：四个槽满了，生产者先等空位；四个槽空了，消费者先等工作；关闭以后拒绝新工作，但把已经接受的项交完再结束。计算在取出以后执行，不把业务回调放在队列锁里。

从 K60 的同一目录继续。本节提供按值保存64位任务标识的用户队列，组合已有互斥与两个变化通知；内核仍只提供既有线程、等待和通知。两生产者、两消费者实际处理48项，另单独观察满队列阻塞、两种关闭等待者、FIFO环绕与失败输出。无需GUI或作者工具支线。

<a id="queue-condition-model"></a>
## 版本不是空位，条件必须在锁下读取

队列保存values[4]、首项下标head、未取数量count和closed。count<4表示可以入队，count>0表示可以取出；这些布尔判断称为谓词，即根据当前状态回答“现在能否继续”。另一个执行者可能紧接着改变它们，所以检查和槽操作必须放在同一guard内，不能检查完就放开锁、稍后再写旧槽。

等待通知分为两份：available要求重查“有项或已关闭”，space要求重查“有空位或已关闭”。它们不代表已有多少任务或预留空位。两个消费者收到同一次广播后，可能只有一项可取；后拿到锁的人仍可能看到空并再次等待。关闭则不再变回开放：put返回-32，get优先排空，只有closed且count=0才返回结束0。

| 操作 | 成功结果 | 当前不能继续 | 关闭后的行为 |
| --- | --- | --- | --- |
| put | 0，项已接受入队 | 零预算-11；等待到期-110 | -32，不接受新项 |
| get | 1，并写私人输出 | 零预算-11；等待到期-110，输出不变 | 有项仍取；空则0结束、输出不变 |
| close | 0 | 拿不到锁时按同一预算处理 | 重复关闭0，不重新发布 |

四槽只限制尚在环里排队的项。消费者取走以后，计算仍可能在进行，因此队列容量不是整个系统的在途请求上限。这里保存的是值，不借用槽指针，也不负责指针背后对象的分配、销毁和取消。

先新建 airi_queue.h。初始化宏按结构字段顺序嵌套初始化guard、两个event、四槽数组和三个普通字段，必须在发布以前完成。输出指针属于调用者，必须有效、对齐且不与队列重叠；任意坏映射仍可能触发用户故障。

<!-- write: labs/airios/include/airi_queue.h -->
```c
#ifndef AIRI_QUEUE_H // 有界值队列，不接收借用指针或执行回调。
#define AIRI_QUEUE_H // 包含保护。
#include <airi_mutex.h> // 普通队列字段由一把所有权锁保护。
#include <airi_event.h> // 非空和有空位分别通知，不把通知当任务令牌。
#define AIRI_QUEUE_CAPACITY 4u // 固定四项，只限制排队数量，不代表计算或线程上限。
typedef struct { // 初始化后通过本接口访问，不复制或重置活队列。
    airi_mutex guard; // 保护下面的环位置、个数、关闭状态和槽内容。
    airi_event available, space; // 通知只要求重查非空/非满谓词。
    uint64_t values[AIRI_QUEUE_CAPACITY]; // 队列按值持有64位任务标识，不拥有外部对象。
    unsigned head, count, closed; // head为首项下标，count含尚未取出的项，closed拒绝后续put。
} airi_queue; // 所有使用者结束以前保持同一有效地址。
#define AIRI_QUEUE_INITIALIZER {AIRI_MUTEX_INITIALIZER, AIRI_EVENT_INITIALIZER, AIRI_EVENT_INITIALIZER, {0}, 0, 0, 0} // 在发布以前建立空环和开放状态。
#ifdef __cplusplus // 对未来C++调用保留C符号。
extern "C" { // 不使用宿主容器或线程类型。
#endif // 结束语言差异。
long airi_queue_put(airi_queue *queue, uint64_t value, unsigned ticks); // 0已入队；满且零预算-11，到期-110，关闭-32。
long airi_queue_get(airi_queue *queue, uint64_t *value, unsigned ticks); // 1取得并写私人输出，0关闭且排空；其他失败保持输出，输出不与队列重叠。
long airi_queue_close(airi_queue *queue, unsigned ticks); // 0关闭或原已关闭，唤醒生产/消费者；保留已排队项供排空。
#ifdef __cplusplus // 结束函数的C链接范围。
} // 类型仍按普通聚合布局。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<a id="queue-deadline-and-gap"></a>
## 在检查条件的那次锁里取得版本，再释放等待

设四槽已满，生产者拿着guard读到count=4。它必须在同一临界区里读取space旧版本，然后释放guard，再用这个版本等待。如果反过来，先解锁、另一人取走并通知、自己才读取新版本，就会把已经出现的空位当成旧事，可能等到下一次变化才回来。

取得旧版本后释放，消费者才能取出并发布。发布发生在实际WAIT以前，用户或内核的版本比较会拒绝旧决定；发生在登记以后，则通知会唤醒原调用。无论是哪种返回，都回到take重新持锁，再看count和closed，不能因为0就直接写槽。

每次公开调用只建立一次deadline，take与wait_changed都使用它。零预算不读时钟，只立即尝试锁与条件；暂不能继续统一为-11。正预算按当前tick计算剩余值，重试不延长；到期仍允许一次立即取得并处理已经就绪的状态，但不会继续睡眠。这是等待预算，不是包含所有计算的硬实时完成期限。

新建queue.c，先写prepare、take、release。prepare只建立本次截止，take用剩余预算尝试取得guard；此后持锁的退出路径都借release释放一次，再交出原本结果或释放错误。先完成这三个助手，尚未链接观察程序。

<!-- write: labs/airios/libc/queue.c -->
```c
#include <airi_queue.h> // 四槽环、互斥和两个变化通知。
#include <airi_libc.h> // 同一次名义PIT截止，不借宿主时间。
static long prepare(airi_queue *queue, unsigned ticks, uint64_t *deadline) { // 每个公开操作只建立一个等待期限。
    if (!queue || (uintptr_t)queue % _Alignof(airi_queue) || ticks > 6000) return -22; // 拒绝显式坏形状/预算，任意坏映射仍可能用户故障。
    uint64_t now = ticks ? airi_ticks() : 0; // 零预算不需要时钟，正预算覆盖锁和条件等待的重试。
    if (now > UINT64_MAX - ticks) return -75; // 不让截止加法回绕。
    *deadline = now + ticks; // 私人栈输出只在成功后使用。
    return 0; // 此时尚未访问队列普通字段。
}
static long take(airi_queue *queue, unsigned ticks, uint64_t deadline) { // 重新取得后才检查普通谓词。
    uint64_t now = ticks ? airi_ticks() : 0; // 其他执行者运行同样会消耗原预算。
    unsigned remaining = now < deadline ? (unsigned)(deadline - now) : 0; // 到期仍允许一次立即CAS，不继续睡眠。
    long result = airi_mutex_lock(&queue->guard, remaining); // 互斥和条件等待共用剩余预算。
    return !ticks && result == -110 ? -11 : result; // 非阻塞操作暂时拿不到锁，也统一为EAGAIN。
}
static long release(airi_queue *queue, long result) { // 所有已经持锁的返回路径都经过一次释放。
    long error = airi_mutex_unlock(&queue->guard); // 不在持锁返回以后借调用者补救。
    return error ? error : result; // 正常持有者/有效对象不应失败，仍不伪造释放成功。
}
```

接着写wait_changed。外层已在guard内看到条件不成立；这个助手在同一把锁内取event版本，再调用release。它返回时已经不持guard：无论是发现版本改变还是收到通知，返回0都只让外层回到take，不能沿用上次读取的count或槽位置。零预算与读版本失败也要先释放，正预算则在释放后重新计算剩余时间。

<!-- append: labs/airios/libc/queue.c -->
```c
static long wait_changed(airi_queue *queue, airi_event *event, unsigned ticks, uint64_t deadline) { // 调用时持guard，返回时不持guard。
    if (!ticks) return release(queue, -11); // 谓词不成立而禁止睡眠，不改队列或输出。
    long expected = airi_event_sequence(event); // 必须在同一次谓词检查的锁内取得旧版本。
    if (expected < 0) return release(queue, expected); // 失败不能窄化成合法快照。
    long error = release(queue, 0); // 确实放开guard以后，另一方才有权改变谓词。
    if (error) return error; // 不在释放失败后继续安排睡眠。
    uint64_t now = airi_ticks(); // 释放和切换消耗的时间也计入原截止。
    if (now >= deadline) return -110; // 相减前拒绝到期。
    return airi_event_wait(event, (uint32_t)expected, (unsigned)(deadline - now)); // 0仅要求外层重新拿锁检查，不能直接操作槽。
}
```

<a id="queue-publish-under-guard"></a>
## 通知可以先发，普通状态仍由guard交接

本版在guard内调用publish，再修改槽和count，最后释放guard。这样版本饱和或通知报错时，本次项还没有入队；不能先写槽再以-75返回，让生产者误以为没有接受而重交一次。

通知先发并不授权读队列。被唤醒者必须先取得guard，原持有者尚未释放时它不能读取普通状态；取得以后才看到已经写完的槽、head、count或closed。发布顺序号的release不负责本次随后写入的普通队列数据，这些数据由guard的释放／取得关系交接。通知与真正状态改变可以不在同一瞬间，条件循环负责重新判断。

这也有代价：若此刻发生调度，醒来者可能先在guard上竞争。本版先保留简单、可说明失败提交点的顺序，后续可以把版本推进与唤醒拆开，减少持锁通知；不能直接把现在的publish移到解锁后，忽略饱和失败时项是否已提交。

put拿锁后先检查closed，再检查空位。空位尾下标为(head+count)%4；例如head=1、count=3时，尾回到0。publish成功才写完整value，再增加count并释放。返回0只表示已接受，不表示消费者已经完成计算。满时走wait_changed(space)，0返回仍从头重查。

<!-- append: labs/airios/libc/queue.c -->
```c
long airi_queue_put(airi_queue *queue, uint64_t value, unsigned ticks) { // 64位值在成功以后归队列保存。
    uint64_t deadline; // 各次重试共享这一份截止。
    long result = prepare(queue, ticks, &deadline); // 失败前没有改变队列。
    if (result) return result; // 参数或加法错误。
    for (;;) { // 每次通知以后都从锁下的谓词重新开始。
        result = take(queue, ticks, deadline); // 不把上次等待返回当成已持有guard。
        if (result) return result; // 当前未持锁。
        if (queue->closed) return release(queue, -32); // 已关闭不接收新值，即使还有空位。
        if (queue->count < AIRI_QUEUE_CAPACITY) { // 只有锁内确认空位才能写环。
            result = airi_event_publish(&queue->available); // 先在guard内发布；版本饱和失败时尚未修改槽，醒来的人仍须取得guard。
            if (result < 0) return release(queue, result); // 版本或通知错误不把本项悄悄入队。
            queue->values[(queue->head + queue->count) % AIRI_QUEUE_CAPACITY] = value; // 首项加数量定位尾槽，环绕不移动其他项。
            ++queue->count; // 内容先完整写入，再增加可见项数，最终由guard释放发布。
            return release(queue, 0); // 已接受，不能再由生产者重复提交同一项。
        }
        result = wait_changed(queue, &queue->space, ticks, deadline); // 满时等待空位变化，释放后不能保留槽的引用。
        if (result) return result; // 到期或错误结束本次操作。
    }
}
```

get先验证私人输出，再拿锁。count非零时，即使closed也先排空：先发布space，再复制首项到输出，head模4前进，count减一，释放后返回1。count为零才检查closed，关闭则返回0且不写输出；开放则等available。错误和超时也不写输出，不能把未初始化值当成一个任务。

<!-- append: labs/airios/libc/queue.c -->
```c
long airi_queue_get(airi_queue *queue, uint64_t *value, unsigned ticks) { // 输出必须私人、有效且不与队列重叠。
    if (!value || (uintptr_t)value % _Alignof(uint64_t)) return -22; // 显式坏输出在取走项以前拒绝。
    uint64_t deadline; // 与put相同的总等待预算。
    long result = prepare(queue, ticks, &deadline); // 仍未修改输出。
    if (result) return result; // 队列形状/预算错误。
    for (;;) { // 醒来以后重新检查非空和关闭，而不是直接读旧槽。
        result = take(queue, ticks, deadline); // 锁仍归同一队列对象。
        if (result) return result; // 未取得，不触碰普通状态。
        if (queue->count) { // 已关闭也先排空已有项。
            result = airi_event_publish(&queue->space); // guard内先验证本次发布能前进，失败不取走值。
            if (result < 0) return release(queue, result); // 输出和环保持原状态。
            *value = queue->values[queue->head]; // 按值复制到调用者自己的输出，不借槽指针。
            queue->head = (queue->head + 1) % AIRI_QUEUE_CAPACITY; // 取走首项后环头前进一格。
            --queue->count; // 与head、输出构成同一次受保护的取出。
            return release(queue, 1); // 1明确区分取得值与结束。
        }
        if (queue->closed) return release(queue, 0); // 关闭且排空才结束，输出仍保持调用前内容。
        result = wait_changed(queue, &queue->available, ticks, deadline); // 空而开放才等待后续入队或关闭。
        if (result) return result; // 负返回不改输出。
    }
}
```

close持同一guard，原已关闭直接返回0。第一次先对available和space分别发布，让空队列的消费者和满队列的生产者都能醒来重查，再提交closed=1。两次通知之间仍持锁，醒来者不能读取半完成的关闭。

如果第一份发布成功、第二份失败，closed仍未提交。可能已有一次无状态变化通知，但K60规定返回后要重查，因此不构成伪造关闭。已有项也不会被清空。正常成功后，等待put的人看到closed而返回-32，等待get的人看到空才结束；close本身不销毁对象，所有worker仍需join后才能结束队列寿命。

<!-- append: labs/airios/libc/queue.c -->
```c
long airi_queue_close(airi_queue *queue, unsigned ticks) { // 关闭是广播重查，不删除已经接受的工作。
    uint64_t deadline; // 这里只可能等待guard。
    long result = prepare(queue, ticks, &deadline); // 尚未读取普通closed。
    if (result) return result; // 参数失败不关闭。
    result = take(queue, ticks, deadline); // 串行处理与其他操作的交接。
    if (result) return result; // 未持锁。
    if (queue->closed) return release(queue, 0); // 重复关闭不额外推进版本。
    result = airi_event_publish(&queue->available); // 空队列的消费者也须醒来观察结束。
    if (result >= 0) result = airi_event_publish(&queue->space); // 满队列的生产者须醒来观察拒绝后续put。
    if (result < 0) return release(queue, result); // 可能已经有一次无状态变化通知，closed仍不提交。
    queue->closed = 1; // 两类通知成功后在同一guard下提交关闭。
    return release(queue, 0); // 醒来者拿到guard以后看到最终状态。
}
```

<a id="queue-observation-roles"></a>
## 先验证一个槽的交接，再运行多生产消费

新建queue-probe.c。probe观察空满和关闭等待者，draining观察关闭后排空，jobs跑真正工作负载，limited单独观察饱和前不提交。started/completed仅握手，不替库读取或修改队列内部状态；sum、squares和seen在取值后原子累计。

<!-- write: labs/airios/queue-probe.c -->
```c
#include <airi_queue.h> // 同一有界队列实现，不借宿主队列。
#include <airi_threads.h> // 真正共享地址空间的工作执行者。
#include <airi_libc.h> // 时钟、串口和主动交接。
#include <stdatomic.h> // 夹具进度和锁外计算结果独立发布。
static airi_queue probe = AIRI_QUEUE_INITIALIZER, draining = AIRI_QUEUE_INITIALIZER, jobs = AIRI_QUEUE_INITIALIZER, limited = AIRI_QUEUE_INITIALIZER; // 各观察对象有独立生命周期。
static _Atomic unsigned started, completed; // 只协调观察，不读写队列内部状态。
static _Atomic uint64_t seen, sum, squares; // 每项在队列之外计算并累计。
static void require(int condition, const char *message) { // 任意失败结束整个真实用户进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(87); } // 不继续输出成功标记。
}
static void await(_Atomic unsigned *word, unsigned value) { // 夹具握手有独立总期限。
    uint64_t start = airi_ticks(); // 同一个名义PIT时钟。
    while (atomic_load_explicit(word, memory_order_acquire) < value) { // 等真实工作者的发布。
        require(airi_ticks() - start < 100, "queue handshake timeout"); airi_yield(); // 不用无限轮询掩盖阻塞。
    }
}
```

blocked_producer只向已经满的probe提交5，原调用一直等待到空位出现，结束时发布completed。closing_consumer在空probe上等get，只接受关闭后的0，私人输出99保持不变。closing_producer借用draining的地址提交10，主保证对象活到join以后；它应因关闭返回-32，不能变成一个已接受的项。

<!-- append: labs/airios/queue-probe.c -->
```c
static long blocked_producer(void *argument) { // 队列四槽已满以后才创建。
    (void)argument; // 本轮固定追加值5。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 只是已开始，不伪造内核等待登记。
    long result = airi_queue_put(&probe, 5, 200); // 主取走一项以后才有空位。
    atomic_store_explicit(&completed, 1, memory_order_release); // 即使错误也发布结束，再由join检查结果。
    return result; // 0才表示确实接受。
}
static long closing_consumer(void *argument) { // 空队列上等待关闭广播。
    (void)argument; uint64_t value = 99; // 私人输出，EOF应保持99。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 主等待两人都开始。
    long result = airi_queue_get(&probe, &value, 200); // 本轮没有新项，只会关闭。
    return result == 0 && value == 99 ? 0 : -240; // 结束不是一个伪造任务值。
}
static long closing_producer(void *argument) { // 满队列上的生产者必须被关闭广播唤醒并拒绝。
    airi_queue *queue = argument; // 主创建时借用draining，保持到join以后。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 只表示已经开始尝试。
    return airi_queue_put(queue, 10, 200); // 关闭以后原调用返回-32，不能偷偷接受第十项。
}
```

负载的两个producer各提交24个不重叠ID：序号0产生1至24，序号1产生25至48。consumer每次get成功以后才处理私人值，把ID对应位设入64位seen；fetch_or返回的旧位已存在就说明重复取出。再累加值与平方，yield也在队列锁外。每个消费者取得项数可以不同，只要求两人的总数48，不把调度公平当作已实现。

任务48对应第47位，所有位都在64位范围内。最终位图为低48位全1，数值和为1176，平方和为38024。这些是实际工作结果；FIFO顺序另外用单一生产者的probe验证，不宣称并发两生产者一定按数值大小入队。

<!-- append: labs/airios/queue-probe.c -->
```c
static long producer(void *argument) { // 两人各产生连续但互不重叠的24个任务号。
    unsigned index = (unsigned)(uintptr_t)argument; // 创建时传入0或1。
    for (unsigned i = 1; i <= 24; ++i) // 总共48项，均适合64位已见位图。
        if (airi_queue_put(&jobs, index * 24 + i, 200)) return -241; // 背压时真实等待，不覆盖仍未取走的槽。
    return 24; // 主join以后才关闭，不把尚未入队任务丢掉。
}
static long consumer(void *argument) { // 取值以后在队列临界区外计算。
    (void)argument; long count = 0; // 每人完成数可以不同，不承诺公平分配。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 工作负载也先让两消费者等待空队列。
    for (;;) { // 直到已关闭且排空。
        uint64_t value; // 私人输出，不持有队列槽地址。
        long result = airi_queue_get(&jobs, &value, 200); // 1得到任务，0结束，负数失败。
        if (result == 0) return count; // 不能把EOF计成一次工作。
        if (result != 1 || value < 1 || value > 48) return -242; // 失败时不读取无效输出。
        uint64_t bit = 1ull << (value - 1); // 任务1对应最低位，任务48不越过64位宽度。
        if (atomic_fetch_or_explicit(&seen, bit, memory_order_relaxed) & bit) return -243; // 旧位已存在说明同一项被重复消费。
        atomic_fetch_add_explicit(&sum, value, memory_order_relaxed); // 普通算术位于queue_get返回以后。
        atomic_fetch_add_explicit(&squares, value * value, memory_order_relaxed); // 一项有限计算，不在队列锁内执行回调。
        ++count; airi_yield(); // 主动交接也在锁外，其他人可以继续进出队列。
    }
}
```

<a id="queue-failure-and-backpressure"></a>
## 到期保留输出，满时停止接受第五项

main从输出哨兵71开始。坏输出和预算返回-22，空且零预算为-11；空队列给三tick则真实等待并到期，输出仍为71。这里的空是暂时没有工作，尚不是EOF。

<!-- append: labs/airios/queue-probe.c -->
```c
int main(void) { // 先逐项展示失败和关闭，再跑实际生产消费。
    uint64_t value = 71; long result; // 所有失败与EOF都应保留私人哨兵。
    require(airi_queue_get(&probe, 0, 0) == -22 && airi_queue_put(&probe, 9, 6001) == -22, "queue invalid arguments"); // 错误参数不进入队列操作。
    require(airi_queue_get(&probe, &value, 0) == -11 && value == 71, "queue empty nonblocking"); // 空且不等待不是EOF。
    uint64_t start = airi_ticks(); // 空队列真实等待三tick。
    require(airi_queue_get(&probe, &value, 3) == -110 && value == 71 && airi_ticks() - start >= 3, "queue empty timeout"); // 超时不写输出。
```

接着放入1、2、3、4，占满四槽。尝试提交9的零预算与三tick操作分别返回-11、-110，都不得覆盖旧项。创建worker提交5，主等待它已开始并睡三tick，确认completed仍为0；只有主get取走1，原put才恢复成功。随后依次取出2、3、4、5，既验证尾槽环绕，也验证两次失败的9没有半入队。

这就是背压：容量不足时不继续接受、覆盖或无限分配。等待中的生产者仍持有自己尚未被接受的值，队列没有偷偷取得该值所有权。

<!-- append: labs/airios/queue-probe.c -->
```c
    airi_puts("[QUEUE_BACKPRESSURE_BEGIN]\n"); // 宿主独立核对满队列上的实际等待。
    for (uint64_t i = 1; i <= 4; ++i) require(airi_queue_put(&probe, i, 0) == 0, "queue initial fill"); // 正好占满四槽。
    require(airi_queue_put(&probe, 9, 0) == -11, "queue full nonblocking"); // 不覆盖首项。
    start = airi_ticks(); require(airi_queue_put(&probe, 9, 3) == -110 && airi_ticks() - start >= 3, "queue full timeout"); // 到期仍不接收9。
    long blocked = airi_thread_create(blocked_producer, 0); require(blocked > 0, "queue blocked producer create"); // 第五项由真实worker提交。
    await(&started, 1); require(airi_sleep(3) == 0 && atomic_load_explicit(&completed, memory_order_acquire) == 0, "queue producer blocked"); // 满时尚不能完成。
    require(airi_queue_get(&probe, &value, 0) == 1 && value == 1, "queue opens one slot"); // 取走旧首项后才能接受5。
    require(airi_thread_join(blocked, &result) == 0 && result == 0, "queue producer resumed"); // 原put继续，不要求重新提交。
    for (uint64_t i = 2; i <= 5; ++i) require(airi_queue_get(&probe, &value, 0) == 1 && value == i, "queue FIFO after wrap"); // 环绕后仍按2/3/4/5返回，没有失败提交的9。
    airi_puts("[QUEUE_BACKPRESSURE_OK] capacity=4 resumed=1 fifo=2,3,4,5\n"); // 锁外完成正常路径标记。
```

<a id="queue-close-both-sides"></a>
## 空的一侧结束，满的一侧拒绝，已有工作继续排空

probe已排空后，创建两位closing_consumer并让它们真正等待。close广播后，两人取得guard重查closed/count，返回EOF0而不是一个伪造值。重复close成功，不重新开放；新put返回-32，后续get仍为0并保持私人输出。

<!-- append: labs/airios/queue-probe.c -->
```c
    atomic_store_explicit(&started, 0, memory_order_relaxed); // 已join，没有旧worker访问进度。
    airi_puts("[QUEUE_CLOSE_WAITERS_BEGIN]\n"); // 后面两位应真正等待空队列。
    long first = airi_thread_create(closing_consumer, 0), second = airi_thread_create(closing_consumer, 0); // 同一开放空队列。
    require(first > 0 && second > 0, "queue closing consumers"); // 不能用单人冒充广播。
    await(&started, 2); require(airi_sleep(3) == 0 && airi_queue_close(&probe, 200) == 0, "queue close wakes consumers"); // closed在guard内发布，两人重查得到EOF。
    require(airi_thread_join(first, &result) == 0 && result == 0 && airi_thread_join(second, &result) == 0 && result == 0, "queue close results"); // 两个私人输出都仍为99。
    require(airi_queue_close(&probe, 0) == 0 && airi_queue_put(&probe, 8, 0) == -32, "queue closed rejects put"); // 重复关闭成功但不重新开放。
    value = 71; require(airi_queue_get(&probe, &value, 0) == 0 && value == 71, "queue EOF unchanged"); // EOF与EAGAIN区分。
    airi_puts("[QUEUE_CLOSE_WAITERS_OK] consumers=2 eof=unchanged\n"); // 本轮真实等待和广播已经收拢。
```

draining先接受6、7、8、9，占满以后再创建closing_producer提交10。主让它登记到space，然后关闭；原put必须被唤醒并返回-32。已接受的6至9仍按FIFO全部取出，最后才EOF。这样两份关闭通知都有实际等待者路径，不只在代码中写了两次publish。

<!-- append: labs/airios/queue-probe.c -->
```c
    for (uint64_t i = 6; i <= 9; ++i) require(airi_queue_put(&draining, i, 0) == 0, "queue drain fill"); // 独立队列拥有四个已接受值。
    atomic_store_explicit(&started, 0, memory_order_relaxed); // 先前两位关闭消费者均已join。
    airi_puts("[QUEUE_CLOSE_PRODUCER_BEGIN]\n"); // 宿主核对满队列另一侧的关闭通知。
    blocked = airi_thread_create(closing_producer, &draining); require(blocked > 0, "queue closing producer create"); // 借用对象地址，不转移队列所有权。
    await(&started, 1); require(airi_sleep(3) == 0, "queue closing producer registration"); // 确实有机会登记到space键。
    require(airi_queue_close(&draining, 200) == 0 && airi_queue_put(&draining, 10, 0) == -32, "queue drain closed"); // 关闭不清空已接受工作。
    require(airi_thread_join(blocked, &result) == 0 && result == -32, "queue closed producer rejected"); // 原等待调用被唤醒以后重查closed。
    airi_puts("[QUEUE_CLOSE_PRODUCER_OK] blocked=1 rejected=closed\n"); // 只有真实返回-32才输出。
    for (uint64_t i = 6; i <= 9; ++i) require(airi_queue_get(&draining, &value, 0) == 1 && value == i, "queue drains before EOF"); // 先排空再结束。
    require(airi_queue_get(&draining, &value, 0) == 0 && value == 9, "queue drained EOF"); // EOF保持最后一次私人输出。
```

limited尚未被交给任何worker，夹具把它的available初值设为UINT32_MAX；put7返回-75，随后零预算get仍为-11且私人输出保持9，证明饱和在槽提交前被报告。这个对象不重置、不宣称能继续无限发布，寿命随程序结束。

<!-- append: labs/airios/queue-probe.c -->
```c
    limited.available.sequence = UINT32_MAX; // 尚未发布给worker的独立夹具初值，不重置活对象。
    require(airi_queue_put(&limited, 7, 0) == -75 && airi_queue_get(&limited, &value, 0) == -11 && value == 9, "queue version failure before mutation"); // 饱和在修改槽以前报告，没有半入队。
```

<a id="queue-workload"></a>
## 生产结束才关闭，取出以后再计算

最后先启动两consumer，空时让它们实际登记；再启动两producer。主join两producer各返回24，才知道48项都已接受，然后close。此时consumer可能还在计算，环里也可能有剩余项，关闭不会丢弃它们。主再join两consumer，汇总已完成数、seen位图、和值和平方和。

当前最多四位负载worker同时存在，先前观察均已join，不超出全局八工作槽。队列自身不创建线程，也不等待计算结果；主分别管理“全部提交”和“全部计算完成”的两个节点。这个区分以后会继续用于线程池与Server-Lib的请求生命周期。

<!-- append: labs/airios/queue-probe.c -->
```c
    atomic_store_explicit(&started, 0, memory_order_relaxed); // 前组worker已经全部join。
    long consumers[2], producers[2]; // 两种角色分别收取，不固定每消费者拿到多少项。
    for (unsigned i = 0; i < 2; ++i) { consumers[i] = airi_thread_create(consumer, 0); require(consumers[i] > 0, "queue workload consumer"); } // 先启动实际等待者。
    await(&started, 2); require(airi_sleep(3) == 0, "queue workload initially empty"); // 无工作时不忙等消耗CPU。
    for (unsigned i = 0; i < 2; ++i) { producers[i] = airi_thread_create(producer, (void *)(uintptr_t)i); require(producers[i] > 0, "queue workload producer"); } // 每人固定24个不同ID。
    for (unsigned i = 0; i < 2; ++i) require(airi_thread_join(producers[i], &result) == 0 && result == 24, "queue workload produced"); // 等全部接受以后才能关闭。
    require(airi_queue_close(&jobs, 200) == 0, "queue workload close"); // 尚在环中的项继续由消费者排空。
    long total = 0; // 本人已结束的结果相加，不读运行中的普通局部值。
    for (unsigned i = 0; i < 2; ++i) { require(airi_thread_join(consumers[i], &result) == 0 && result >= 0, "queue workload consumed"); total += result; } // 不承诺公平分配。
    require(total == 48 && atomic_load_explicit(&seen, memory_order_relaxed) == ((1ull << 48) - 1), "queue exactly once"); // 每个1至48ID均出现且没有重复。
    require(atomic_load_explicit(&sum, memory_order_relaxed) == 1176 && atomic_load_explicit(&squares, memory_order_relaxed) == 38024, "queue outside lock computation"); // 1至48和及平方和实算。
    airi_puts("[QUEUE_USER_OK] capacity=4 jobs=48 exactly_once=1 sum=1176 squares=38024 backpressure=wait close=drain timeout=preserved\n"); // 所有失败、关闭与工作结果完成后才发布。
    return 0; // shell收取真实成功结果。
}
```

<a id="queue-build-evidence"></a>
## 编译库与观察入口，再检查真实等待

queue-lib.o显式编译libc/queue.c，观察主程序命名queue-probe.c，避免既有模式规则把同名queue.o选成库源码。最终queue.elf链接普通mutex、event、线程库与完整main；不链接观察暂停锁。再打包镜像并注册queue。

<!-- append: Makefile -->
```makefile

# 队列库只依赖自有用户锁和变化通知。
$(OUT)/user/queue-lib.o: labs/airios/libc/queue.c Makefile
# 与观察main的queue-probe.o分别命名。
	@mkdir -p $(dir $@)
# 仍为无宿主libc的静态用户目标。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 有界生产消费使用普通锁，不链接暂停观察版。
$(OUT)/user/queue.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/event-lib.o $(OUT)/user/queue-lib.o $(OUT)/user/queue-probe.o labs/airios/program.ld
# 全部执行者运行在同一个实际用户ELF与共享页表中。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 目录只借完成链接的真实文件。
$(OUT)/labs/airios/images.o: $(OUT)/user/queue.elf
# 路径只交给镜像打包，不读取主机业务结果。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_QUEUE_ELF='"$(OUT)/user/queue.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 有界生产消费的独立用户映像。 */
.global airi_queue_elf_start, airi_queue_elf_end /* 初始目录只借只读ELF范围。 */
airi_queue_elf_start: /* 从完整文件头开始。 */
.incbin AIRI_QUEUE_ELF /* 真实链接的自有队列程序。 */
airi_queue_elf_end: /* 右端不含字节。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

宿主分别切出背压、关闭消费者、关闭生产者三个范围，要求实际FUTEX_WAIT以及对应通知人数。八次工作者创建和返回都按本条命令的真实PID核对：一背压、两关闭消费者、一关闭生产者和四负载。最终成功还须guest的位图及计算断言成立，不用宿主替guest计算并伪装结果。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert [expected for pid,tid,address,expected in event_waits].count('1')==5 and event_waits[-1][3]=='2' # 最后新快照不会领取旧通知。
                event_wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=16 woken=(\d+)',events_output) # 饱和对象末次发布、先发布、无变化通知、最后广播。
                assert event_wakes==['0','0','2','2'] # 拒绝回绕不发通知；两次真正广播分别唤醒两人。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                assert [expected for pid,tid,address,expected in event_waits].count('1')==5 and event_waits[-1][3]=='2' # 最后新快照不会领取旧通知。
                event_wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=16 woken=(\d+)',events_output) # 饱和对象末次发布、先发布、无变化通知、最后广播。
                assert event_wakes==['0','0','2','2'] # 拒绝回绕不发通知；两次真正广播分别唤醒两人。
                offset=len(text); wire.sendall(b'queue\n'); text=wait('[SHELL_COMMAND_DONE] name=queue code=0',offset) # 真正四槽生产消费、背压与关闭排空。
                queue_output=text[offset:] # 不能用宿主统计或旧命令满足。
                assert '[QUEUE_USER_OK] capacity=4 jobs=48 exactly_once=1 sum=1176 squares=38024 backpressure=wait close=drain timeout=preserved' in queue_output # guest位图、结果与错误返回已逐一核对。
                backpressure=queue_output.split('[QUEUE_BACKPRESSURE_BEGIN]',1)[1].split('[QUEUE_BACKPRESSURE_OK]',1)[0] # 满队列超时和第五项真实等待发生在这里。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',backpressure))>=2 and 'result=-110' in backpressure and re.search(r'\[FUTEX_WAKE\].*woken=1',backpressure) # 不用主动yield冒充背压。
                closing=queue_output.split('[QUEUE_CLOSE_WAITERS_BEGIN]',1)[1].split('[QUEUE_CLOSE_WAITERS_OK]',1)[0] # 关闭必须唤醒两位空队列消费者。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',closing))>=2 and re.search(r'\[FUTEX_WAKE\].*count=16 woken=2',closing) # 结束状态由真实等待返回后重查。
                producer_close=queue_output.split('[QUEUE_CLOSE_PRODUCER_BEGIN]',1)[1].split('[QUEUE_CLOSE_PRODUCER_OK]',1)[0] # 满队列关闭必须唤醒尚未接受的新项。
                assert re.search(r'\[FUTEX_WAIT\] pid=',producer_close) and re.search(r'\[FUTEX_WAKE\].*count=16 woken=1',producer_close) # 与空队列关闭的消费者路径分别实测。
                queue_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=queue ',queue_output) # 只数本次队列进程的执行者。
                assert queue_owner # 工作链共享同一PID。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 and len(re.findall(r'\[THREAD_RETURN\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 # 一背压、二关闭消费者、一关闭生产者及四负载均收拢。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现闭合，运行：

<!-- run -->
```sh
./airios build  # 静态链接普通锁、通知、队列库与真实工作负载
```

<!-- run -->
```sh
./airios test  # 背压、空满超时、两侧关闭和48项实际计算
```

<a id="queue-results"></a>
## 接受、等待、取走和完成，各有自己的状态

交互执行`./airios run`、`run sh`、`queue`，本轮实际输出：

```text
[QUEUE_BACKPRESSURE_OK] capacity=4 resumed=1 fifo=2,3,4,5
[QUEUE_CLOSE_WAITERS_OK] consumers=2 eof=unchanged
[QUEUE_CLOSE_PRODUCER_OK] blocked=1 rejected=closed
[QUEUE_USER_OK] capacity=4 jobs=48 exactly_once=1 sum=1176 squares=38024 backpressure=wait close=drain timeout=preserved
[SHELL_COMMAND_DONE] name=queue code=0
```

满时的等待没有取得空位承诺，通知返回仍要争取guard和重查；put返回0只表示环已保存值，get返回1表示值已移到消费者私人输出，计算完成再由worker结果体现。close拒绝新项而排空旧项，EOF不写输出，也不销毁仍有使用者的锁和通知对象。

这里仍是固定四槽和有限32位通知代次，没有扩容、取消、detach、公平或robust恢复。用户故障会结束所属进程组，不把某个死worker留下的队列当成可恢复的生产级线程池。普通锁支持当前单CPU调度，整个内核尚非SMP；Linux、网络和C++运行时仍有后续工作。下一步继续把这些真实生命周期接到POSIX线程源码接口，而非为缺失功能返回伪成功。
