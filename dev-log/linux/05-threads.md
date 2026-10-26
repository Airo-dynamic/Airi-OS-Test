# L05 · 条件变量等谁改变谓词，futex怎样避免睡过一次发布

从K32和L04继续。AiriOS用单CPU、不抢占内核和持久条件登记等待；Linux工作线程却可以同时运行，不能把“检查完再等”拆成不受保护的两步。本节先写容量二的任务队列，让两个pthread消费者处理二十个任务，再单独用一个四字节原子词连接用户发布和Linux futex等待。

<a id="threads-queue-predicate"></a>
## 条件变量不保存任务，任务和关闭状态在队列里

消费者面对空队列，不能先解锁、再登记等待：生产者可能恰好在两者之间放入任务并通知，消费者随后睡下，再也没有人叫它。让检查、释放锁与等待正确交接，才是这里引入条件变量的原因。queue 保存两个整数、head/count 和 closed，由同一 mutex 保护。生产者先锁住，满且未关闭时在while里等not_full；消费者先锁住，空且未关闭时在while里等not_empty。pthread_cond_wait把释放mutex和进入等待作为交接，返回时重新持锁；别人可能先消费了任务，也可能出现虚假唤醒，所以返回后仍重查谓词，不能改成只判断一次的if。

被通知回来也不是取得了一张“任务归我”的凭证；任务仍在共享队列中，需要重新持锁确认。写入任务、增加 count 和通知发生在锁内；解锁后消费者取得同一锁，才读取并取走对应任务。signal不是一个可累计的“任务数量”，错过signal也不该丢掉已经变真的count条件。生产者结束时设closed并broadcast，消费者先排空已有任务，再在closed且empty时退出；不能一看到closed就丢弃尾部工作。

队列保护的是任务交接，统计与线程结束又有自己的同步关系。两名消费者各写自己的 worker 结果，main在join后读取。completed是独立统计，两个线程用relaxed原子加一，防止增量相互覆盖；它不负责发布queue数组，也不能代替mutex或join来判断普通结果字段已经可安全读取。短暂模拟工作放在锁外，避免拿着队列锁睡眠。

pthread接口返回错误码本身，所以check_thread先把这个返回码交给perror，不借用陈旧errno。getpid相同、gettid不同的输出用来辨认内核调度线程；pthread_t是库的句柄，不等于这个整数TID。

<a id="threads-futex-publication"></a>
## 数据先写，release发布，acquire后再读

条件变量由 pthread 帮忙完成等待交接。接下来把同一缺口缩到一个词：等待者看到 flag=0 后准备睡，发布者可能已经把它改为 1 并发过唤醒。futex 让内核把“值仍等于 0”与入睡放在一起判断。第二个实验只有一次发布：普通payload写42，随后把atomic_int flag从0以release存成1。等待者用acquire加载，确实看到1之后才读payload，这建立普通数据的可见关系。WAKE只负责让可能睡眠的内核线程重新获得运行机会，不替代C语言的release/acquire；volatile也不提供这种线程同步。

但避免睡过通知，只保证线程会再获得执行机会；payload 是否可安全读取，仍由前面的 release/acquire 关系承担。futex_word 要求四字节大小和对齐，并检查当前目标atomic_int为lock-free。C代码仍用原子接口访问，内核通过SYS_futex读取同一个词。FUTEX_WAIT_PRIVATE要求内核只在词仍为期望值0时原子入睡。若发布发生在用户load之后、实际等待之前，内核看见1会返回EAGAIN，等待者重查后继续，不会睡过已经过去的WAKE。若已经入睡，后续WAKE让它回来，但它仍需重查条件。

waiter_ready只安排演示开始时序，不保证线程已经睡着；二十毫秒延迟鼓励观察，不是正确性的前提。wait_calls记录实际发出WAIT调用次数，actually_woken记录WAKE真正唤醒数，后者可以为0，不能硬写成1。等待用CLOCK_MONOTONIC总截止两秒，每次EAGAIN或EINTR后只把剩余跨度交给相对WAIT，不不断重置预算。

**新建 `labs/linux/p5-threads/threads.c`。** 两个实验的共享状态与寿命分别明确，完整错误出口一并敲入。Mac只编译拒绝分支，Linux路径必须在Linux构建。

<!-- write: labs/linux/p5-threads/threads.c -->
```c
#define _GNU_SOURCE // Linux syscall与线程接口声明。
#include <stdio.h> // 非Linux分支也可明确报告限制。
#ifndef __linux__ // Mac编译只检查环境拒绝路径。
int main(void) { // 不把宿主pthread当Linux futex实验。
    fputs("Linux is required for pthread/futex observation.\n", stderr); // 明确目标环境。
    return 77; // 未验证，不是通过。
}
#else // 完整机制仅在Linux编译运行。
#include <errno.h> // syscall失败与pthread返回错误码分别处理。
#include <linux/futex.h> // Linux等待/唤醒操作，不是C标准库常量。
#include <pthread.h> // Linux用户线程与条件变量。
#include <sched.h> // 准备阶段让出CPU，不在未准备时无限自旋。
#include <stdatomic.h> // C11发布/获取与独立统计计数。
#include <stdint.h> // 时间与futex字宽。
#include <stdlib.h> // 失败结束整个独立实验。
#include <sys/syscall.h> // SYS_futex和真正内核TID。
#include <time.h> // 单调截止时间和有界模拟工作。
#include <unistd.h> // syscall、进程ID。
static void fail(const char *operation) { // errno必须来自具体失败操作。
    perror(operation); // 保存系统错误身份。
    exit(1); // 运行器不发布成功。
}
static void check_thread(int error, const char *operation) { // pthread返回错误码本身，不自动设置errno。
    if (error) { // 只有非零才失败。
        errno = error; // 给perror一个明确来源。
        fail(operation); // 不用陈旧errno解释线程失败。
    }
}
static void expect(int condition, const char *reason) { // 事实不符与系统errno分开。
    if (!condition) { // 避免无效继续。
        fprintf(stderr, "expectation failed: %s\n", reason); // 保留当前机制位置。
        exit(1); // 无成功记录。
    }
}
static int64_t now_ns(void) { // 相对单调时间，不受墙钟调整。
    struct timespec now; // 秒与纳秒两个字段。
    if (clock_gettime(CLOCK_MONOTONIC, &now)) // 当前机器的真实时钟。
        fail("clock_gettime"); // 不使用未初始化值。
    return (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec; // 实验时间范围内转换成纳秒。
}
static void pause_ns(long ns) { // 有界工作延迟，不在互斥区里睡眠。
    struct timespec remaining = {0, ns}; // 此实验只用小于一秒的常量。
    while (nanosleep(&remaining, &remaining)) // 信号中断保留剩余时间。
        if (errno != EINTR) // 其他错误不能忽略。
            fail("nanosleep"); // 不伪造已完成延迟。
}
struct queue { // 两个消费者共用有界任务值，不借生产者栈指针。
    pthread_mutex_t mutex; // 保护数组、head/count和关闭谓词。
    pthread_cond_t not_empty, not_full; // 通知只提示重查谓词，不保存任务。
    unsigned values[2], head, count, producer_waits, consumer_waits; // 容量二使背压可见。
    int closed; // 生产结束后仍要先消费残余任务。
};
static struct queue queue = {.mutex = PTHREAD_MUTEX_INITIALIZER, .not_empty = PTHREAD_COND_INITIALIZER, .not_full = PTHREAD_COND_INITIALIZER}; // 线程创建前已初始化。
static atomic_uint completed; // 独立遥测计数，不负责发布队列数据。
struct worker { unsigned id, count, sum; }; // 每个线程写自己的结果，main在join后读取。
static void *consume(void *argument) { // pthread入口返回void*，参数寿命覆盖join。
    struct worker *worker = argument; // 只借main的稳定数组项。
    printf("[WORKER] pid=%ld tid=%ld worker=%u\n", (long)getpid(), syscall(SYS_gettid), worker->id); // 同PID不同内核TID，不把pthread_t当整数PID。
    for (;;) { // 直到关闭并排空，不因一条通知就假设有任务。
        check_thread(pthread_mutex_lock(&queue.mutex), "lock consumer"); // 读取谓词前先拿同一互斥锁。
        while (!queue.count && !queue.closed) { // 空且尚有未来生产者才需要等。
            ++queue.consumer_waits; // 此计数也由mutex保护。
            check_thread(pthread_cond_wait(&queue.not_empty, &queue.mutex), "wait not_empty"); // 原子释放锁并等待，返回时重新持锁。
        }
        if (!queue.count && queue.closed) { // 关闭后已排空，才能退出。
            check_thread(pthread_mutex_unlock(&queue.mutex), "unlock finished consumer"); // 不带锁结束线程。
            break; // main的join随后收拢结果。
        }
        unsigned value = queue.values[queue.head]; // 任务值在当前临界区内取得。
        queue.head = (queue.head + 1) % 2; --queue.count; // 消费一个元素并释放容量。
        check_thread(pthread_cond_signal(&queue.not_full), "signal not_full"); // 唤醒生产者重查容量。
        check_thread(pthread_mutex_unlock(&queue.mutex), "unlock consumer"); // 模拟工作在锁外。
        pause_ns(1000000); // 一毫秒处理时间，给另一个线程和生产者运行机会。
        worker->sum += value; ++worker->count; // 仅当前线程写这项局部结果。
        atomic_fetch_add_explicit(&completed, 1, memory_order_relaxed); // 两消费者的统计增量不可丢，但不发布任务内容。
    }
    return NULL; // join只等完成，不把局部栈地址交回。
}
_Alignas(4) static atomic_int futex_word; // Linux要求四字节对齐，C对象仍通过原子接口访问。
static atomic_int waiter_ready; // 仅用于准备时序，和实际发布flag分开。
static int payload; // 由futex_word的release/acquire关系发布，不靠WAKE本身。
struct wait_result { unsigned calls; int value; }; // waiter独占写，main在join后检查。
_Static_assert(sizeof(atomic_int) == sizeof(uint32_t), "Linux futex word must be 32 bits"); // 固定目标不接受额外原子表示。
static void *wait_value(void *argument) { // 同一进程共享futex_word物理对象。
    struct wait_result *result = argument; // 结果寿命由main持到join。
    int64_t deadline = now_ns() + 2000000000ll; // 整个等待最多两秒，不因EINTR反复重置。
    atomic_store_explicit(&waiter_ready, 1, memory_order_release); // 告诉主线程已到准备位置，不替代实际futex比较。
    while (atomic_load_explicit(&futex_word, memory_order_acquire) == 0) { // 每次返回都重查数据发布条件。
        int64_t remaining = deadline - now_ns(); // 只把剩余时间交给相对WAIT。
        expect(remaining > 0, "futex wait deadline expired"); // 无事件不能永远等。
        struct timespec timeout = {remaining / 1000000000ll, remaining % 1000000000ll}; // Linux FUTEX_WAIT使用相对跨度。
        ++result->calls; // 实际进入syscall前才计数。
        long value = syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL, 0); // 内核比较word仍为0才原子入睡。
        if (value < 0 && errno != EAGAIN && errno != EINTR) // EAGAIN表示发布已抢先发生，不是丢失唤醒。
            fail("FUTEX_WAIT_PRIVATE"); // 超时或其他真实错误明确失败。
    }
    result->value = payload; // acquire看到1之后才读普通共享内容。
    return NULL; // 不把payload指针跨越已结束线程返回。
}
int main(void) { // 所有线程只修改本独立进程内的演示状态。
    atomic_init(&completed, 0); // 在任何消费者启动前初始化。
    pthread_t threads[2]; // pthread句柄不能等同于gettid整数。
    struct worker workers[2] = {{.id = 1}, {.id = 2}}; // 两份结果由各线程独占写。
    for (unsigned i = 0; i < 2; ++i) // 创建两个内核可调度线程。
        check_thread(pthread_create(&threads[i], NULL, consume, &workers[i]), "pthread_create consumer"); // main数组在join前保持有效。
    for (unsigned value = 1; value <= 20; ++value) { // 明确二十个任务，和为210。
        check_thread(pthread_mutex_lock(&queue.mutex), "lock producer"); // 容量谓词和入队在同一锁内。
        while (queue.count == 2 && !queue.closed) { // 满时等待，虚假/竞争唤醒后仍重新检查。
            ++queue.producer_waits; // 不用计数证明一定发生过内核睡眠。
            check_thread(pthread_cond_wait(&queue.not_full, &queue.mutex), "wait not_full"); // 释放锁给消费者，醒后重新持锁。
        }
        expect(!queue.closed, "queue closed before producer finished"); // 关闭不能继续发布新任务。
        queue.values[(queue.head + queue.count) % 2] = value; ++queue.count; // 数据先写，持锁发布新的谓词。
        check_thread(pthread_cond_signal(&queue.not_empty), "signal not_empty"); // 通知等待者重查。
        check_thread(pthread_mutex_unlock(&queue.mutex), "unlock producer"); // unlock/lock交接也建立数据可见关系。
    }
    check_thread(pthread_mutex_lock(&queue.mutex), "lock close"); // 关闭谓词同样受锁保护。
    queue.closed = 1; // 不再有未来任务，但不删除队列现有内容。
    check_thread(pthread_cond_broadcast(&queue.not_empty), "broadcast close"); // 所有可能睡眠的消费者都要重查关闭。
    check_thread(pthread_mutex_unlock(&queue.mutex), "unlock close"); // 消费者能取得锁继续排空。
    for (unsigned i = 0; i < 2; ++i) // 结束后才读取它们的普通结果字段。
        check_thread(pthread_join(threads[i], NULL), "join consumer"); // 线程对象与结果寿命在这里收拢。
    expect(workers[0].count + workers[1].count == 20 && workers[0].sum + workers[1].sum == 210 && atomic_load_explicit(&completed, memory_order_relaxed) == 20, "bounded queue lost or duplicated work"); // 统计原子不替代前面的队列锁或join。
    printf("[QUEUE_OK] tasks=20 sum=210 producer_waits=%u consumer_waits=%u\n", queue.producer_waits, queue.consumer_waits); // 实际等待次数可能随调度变化。
    check_thread(pthread_cond_destroy(&queue.not_empty), "destroy not_empty"); // 所有线程已结束。
    check_thread(pthread_cond_destroy(&queue.not_full), "destroy not_full"); // 无等待者才可销毁。
    check_thread(pthread_mutex_destroy(&queue.mutex), "destroy mutex"); // 结束同步对象寿命。
    atomic_init(&futex_word, 0); atomic_init(&waiter_ready, 0); // 新的独立发布实验。
    expect(atomic_is_lock_free(&futex_word), "target atomic int is not lock-free"); // 不把带隐藏锁的表示直接交给Linux futex。
    struct wait_result result = {0}; // waiter写，join后main读。
    pthread_t waiter; // 真正等待的内核线程句柄。
    check_thread(pthread_create(&waiter, NULL, wait_value, &result), "create futex waiter"); // 两秒deadline由waiter建立。
    int64_t deadline = now_ns() + 2000000000ll; // 准备阶段自己也有上界。
    while (!atomic_load_explicit(&waiter_ready, memory_order_acquire)) { // 等到waiter准备，不假设它已真正睡着。
        expect(now_ns() < deadline, "waiter did not initialize"); // 不无限自旋。
        sched_yield(); // 给等待线程获得CPU机会。
    }
    pause_ns(20000000); // 鼓励观察真实等待，但不把时间安排当互斥正确性依据。
    payload = 42; // 普通数据写先于发布flag。
    atomic_store_explicit(&futex_word, 1, memory_order_release); // acquire见1后可读取这个payload。
    long woken = syscall(SYS_futex, &futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); // 唤醒最多一个内核等待者，不替代release。
    if (woken < 0) // 系统调用失败保留errno。
        fail("FUTEX_WAKE_PRIVATE"); // 不伪造通知成功。
    check_thread(pthread_join(waiter, NULL), "join futex waiter"); // 所有payload读取结束后main才验证。
    expect(result.value == 42, "release/acquire publication failed"); // 不是靠volatile或WAKE发布普通数据。
    printf("[FUTEX_OK] value=%d wait_calls=%u actually_woken=%ld word=%p\n", result.value, result.calls, woken, (void *)&futex_word); // 快时woken可0，照实记录。
    puts("[LINUX_THREADS_OK] queue=bounded publication=release_acquire telemetry=relaxed"); // 完整路径才发布。
    return 0; // 独立Linux程序成功。
}
#endif // 非Linux只执行明确拒绝分支。
```

<a id="threads-run-trace"></a>
## 区分自己调用的futex与库内部的等待

队列和单词实验都可能出现 futex 系统调用，仅看到这个名字还不能知道是哪一次等待。run.sh 用 -pthread 同时编译和链接，再经strace -f记录所有线程的futex与clone。条件变量、join和stdio内部也可能使用futex，不能把每一条都归到我们手写的flag；FUTEX_OK会记录该原子词地址，结合调用栈和TID辨认。strace本身会改变调度与等待次数，不能拿这个输出当无观察开销的性能测试。

整个程序由GNU timeout限制二十秒，错误或超时不发布passed。缺cc、strace或timeout会打印对应Debian/Ubuntu与Arch/CachyOS修复命令，只提示所需依赖，不自动在个人主机提权安装。环境文件记录真实内核、编译器、跟踪器及可查询的libc版本。

**新建 `run.sh`。** 实际线程与内核等待输出是证据，不能由源码里的预期字符串生成。

<!-- write: labs/linux/p5-threads/run.sh -->
```sh
#!/bin/sh
set -eu # 未定义变量或命令失败都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 固定个人x86-64 Linux观察路径。
    echo 'x86-64 Linux is required for pthread/futex observation.' >&2 # 不用Mac模拟Linux。
    exit 77 # 未验证。
fi # 结束环境检查。
mkdir -p artifacts # 本阶段独立证据目录。
printf '{"passed":false,"phase":"p5-threads"}\n' > artifacts/result.json # 先撤销旧成功。
for required in cc strace timeout; do # 核心编译、观察和运行上界缺一不可。
    if ! command -v "$required" >/dev/null 2>&1; then # 先给具体修复方法，不运行半个实验。
        echo "Missing $required. Debian/Ubuntu: apt install build-essential strace coreutils; Arch/CachyOS: pacman -S base-devel strace coreutils." >&2 # 这里只提示，不擅自提权安装。
        exit 77 # 依赖未满足，不能写passed。
    fi # 当前工具存在才继续。
done # 所需工具都已找到。
uname -a > artifacts/environment.txt # 实际Linux与架构。
cc --version >> artifacts/environment.txt # 实际C11编译器。
strace --version >> artifacts/environment.txt # 跟踪器必须真实存在。
timeout --version >> artifacts/environment.txt # GNU coreutils总运行上界，缺工具不能无限等待。
getconf GNU_LIBC_VERSION >> artifacts/environment.txt 2>&1 || true # 非glibc系统可以缺此项，其他环境事实仍保存。
cc -std=c11 -O0 -g -Wall -Wextra -Werror -pthread threads.c -o threads # 同时编译和链接pthread，使用本机Linux运行时。
if timeout -k 2 20 strace -f -o artifacts/futex.log -e trace=futex,clone ./threads > artifacts/threads.log 2>&1; then # 包括所有线程，整个观察最多二十秒。
    cat artifacts/threads.log # 完成后回显真实输出。
else # 超时、程序错误和工具失败都保留证据。
    status=$? # 不被后续cat覆盖。
    cat artifacts/threads.log >&2 # 具体失败原因。
    exit "$status" # 不发布passed。
fi # 程序正常退出。
grep -q '^\[LINUX_THREADS_OK\]' artifacts/threads.log # 不能仅以strace进程存在当成功。
printf '{"passed":true,"phase":"p5-threads","physical_smp_kernel_implemented":false}\n' > artifacts/result.json # Linux观察不冒充AiriOS多核实现。
```

执行器现在用字典把阶段名映射到源码元组，例如p5-threads对应threads.c与run.sh。tuple(LAB_SOURCES)在这里按字典键生成合法phase选项；args.phase已验证后，再用同一个键取得文件名。这样不把新增实验塞进越来越长的嵌套条件表达式。上传、执行和证据下载另有总时间上限，真正远端程序失败时remote.json也保持verified=false并保存失败状态。

**更新阶段执行器。** 每阶段仍在个人主机独立目录，Linux成功状态只在程序及所需观察实际完成后写出。

<!-- write: scripts/linux_lab.py -->
```python
"""Run the authorized Linux programs on the user's personal Linux host."""  # 从本机发起个人 Linux 主机上的两份实验
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import io     # BytesIO 为内存字节提供文件式读写接口。
import json   # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex  # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tarfile     # 读取或构造 tar 归档，成员路径及类型还需单独检查。
from tools import ROOT  # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。

LAB_SOURCES = { # 阶段名到本阶段完整源码元组，新增实验不再嵌套条件表达式。
    'p1': ('ordinary.c', 'raw_start.S', 'run.sh'), # 普通入口与原始Linux系统调用。
    'p2': ('memory.c', 'run.sh'), # 匿名映射、驻留与权限。
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
}
parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
parser.add_argument('--phase', choices=tuple(LAB_SOURCES), default='p1') # 实验来源与远端目录使用同一阶段名。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid SSH host')  # 无效目标在启动SSH之前拒绝。
ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10',args.host]  # 非交互认证，连接最多十秒，最后元素是单个host参数。
# Relative to SSH's login directory; does not overwrite any other project.
remote='AiriOS-labs/' + args.phase  # 相对个人主机登录目录的独立实验目录。
dest=ROOT/'artifacts'/('linux-' + args.phase) # 每阶段独立保存本轮状态。
dest.mkdir(parents=True, exist_ok=True) # SSH失败也必须有证据目录。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'not yet verified'},indent=2)+'\n') # 先覆盖旧成功身份。
connection=subprocess.run(ssh+[f'mkdir -p {shlex.quote(remote)}'],capture_output=True,text=True,timeout=20) # 非交互连接另有总时间上界。
(dest/'connection.log').write_text(connection.stdout + connection.stderr) # 真实失败输出在复制源码之前也保留。
if connection.returncode: # 未连接就不复制也不声称程序运行。
    (dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'connection failed','exit':connection.returncode},indent=2)+'\n') # 明确本轮未验证。
    raise RuntimeError(f'Linux SSH failed ({connection.returncode}); see {dest}/connection.log') # 路径和失败状态交回统一入口。  # 本地参数列表启动SSH，远端命令文本中的目录由quote保护。
names = LAB_SOURCES[args.phase] # argparse已按字典键限制阶段，取得对应文件名元组。
sources=[ROOT/'labs/linux'/args.phase/name for name in names]  # 列表推导按所选阶段名称构成Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True,timeout=30)  # 列表内*展开本阶段路径字符串，最后一项是远端目标。
execution=subprocess.run(ssh+[f'cd {shlex.quote(remote)} && sh run.sh'],timeout=120)  # 远端先切实验目录，再执行shell；暂不check，失败也先下载证据。
data=subprocess.check_output(ssh+[f'cd {shlex.quote(remote)} && tar -cf - artifacts'],timeout=30)  # tar从标准输出交回归档bytes，-f -表示不写远端归档文件。
dest.mkdir(parents=True,exist_ok=True)  # 确保证据父目录存在。
with tarfile.open(fileobj=io.BytesIO(data)) as tar:  # 把内存归档作为文件交给tarfile，with结束关闭归档。
    tar.extractall(dest,filter='data')  # data过滤限制危险成员，解包到本轮证据目录。
if execution.returncode:  # 非零远端状态意味着实验失败，即使tar传输成功。
    (dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'program or observation failed','exit':execution.returncode},indent=2)+'\n') # 例如缺liburing或运行超时不能保留旧成功。
    raise RuntimeError(f'Linux experiment failed; evidence downloaded to {dest}')  # 原始证据已经带回，再报告失败位置。
result=json.loads((dest/'artifacts/result.json').read_text())  # 实际远端程序写出的JSON恢复成字典。
if result.get('passed') is not True:  # get缺键返回None，只有布尔True才接受。
    raise RuntimeError('Remote validation failed')  # 远端记录不满足成功合同。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':True,'status':'program and requested observations completed'},indent=2)+'\n')  # 记录本次实际host和目录，不把另一主机记录混用。
print(f'Remote evidence downloaded: {dest}')  # 显示本轮证据可查看位置。
```

**更新统一入口。** 增加p5-threads，不改变默认p1或guest构建。

<!-- write: scripts/manage.py -->
```python
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import json      # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex     # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import sys  # 取得当前解释器路径、入口参数及标准错误流。
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
    # GNU Make dry-run is the single source of compiler flags, including headers.
    command=make_command('normal','kernel')  # 局部 command 是 Make 参数列表，不是导入的QEMU command函数。
    command.insert(1,'-Bn')  # 在可执行路径之后插入 -B强制列规则、-n只打印的选项。
    lines=subprocess.check_output(command,cwd=ROOT,text=True).splitlines()  # 读取命令文本并按行拆分，不执行打印出的编译器命令。
    entries=[]  # 保存编译数据库条目，每项对应一份源文件及参数。
    for line in lines:  # 逐条检查 Make 打印的命令。
        if ' -c ' not in line or ' -o ' not in line: continue  # 过滤目录创建与链接命令，只考虑单文件编译。
        tokens=shlex.split(line)  # 按shell引号解析参数边界，列表值随后直接保存。
        if '-c' not in tokens or '-o' not in tokens: continue  # 分词后再确认编译选项，排除仅字符串碰巧匹配的行。
        source=tokens[tokens.index('-c')+1]  # index 找到-c位置，其后一个元素是被编译的源文件路径。
        entries.append({'directory':str(ROOT),'file':str(ROOT/source),'arguments':tokens})  # 保存工作目录、绝对源路径和完整参数列表供 clangd 消费。
    target=ROOT/'build/normal/compile_commands.json'  # 数据库放在普通配置目录，.clangd 从此处读取。
    target.parent.mkdir(parents=True,exist_ok=True)   # 数据库父目录缺失时创建，已有目录保持。
    target.write_text(json.dumps(entries,indent=2)+'\n')  # 列表编码为JSON数组，输出本次真实构建参数。

def build(profile):  # 给指定配置调用同一Make规则，普通配置额外更新编辑器数据库。
    run(make_command(profile))  # 组装配置及工具覆盖，执行完整内核与镜像构建。
    if profile=='normal':  # 测试配置不替换日常编辑参数。
        compilation_database()  # 普通配置才导出clangd数据库。
def inspect_elf(profile='normal'):  # 核对独立内核目标，不接受宿主解释器、动态依赖或未解析符号。
    binary=ROOT/'build'/profile/'airios.elf'  # 定位本次配置的ELF，避免检查另一目录的旧产物。
    readelf=tool('llvm-readelf','llvm')  # 按既定规则找到 LLVM ELF 检查器。
    report=subprocess.check_output([readelf,'-h','-l','-d','-s',str(binary)],text=True)  # 合并ELF头、程序段、动态信息和符号表的文本报告。
    nm=subprocess.check_output([tool('llvm-nm','llvm'),'-u',str(binary)],text=True)  # -u 只列未定义符号，独立内核应没有内容。
    if nm.strip() or 'INTERP' in report or '(NEEDED)' in report:  # 非空未定义项、解释器段或动态依赖任一存在就拒绝。
        raise RuntimeError('Kernel is not a fully resolved freestanding executable')  # 报告不满足完全解析的独立目标约定。
    if 'X86-64' not in report and 'x86-64' not in report:  # 兼顾工具文字大小写，必须出现x86-64机器类型。
        raise RuntimeError('Wrong ELF target')  # 错误机器目标不能作为此次内核成功结果。
    if 'EXEC (Executable file)' not in report:  # 要求固定地址静态ET_EXEC，与链接脚本装载方式一致。
        raise RuntimeError('Expected a static ET_EXEC ELF')  # PIE或其他ELF种类不满足目前启动路径。
    (ROOT/'artifacts').mkdir(exist_ok=True)  # 建立检查报告目录。
    (ROOT/'artifacts/kernel-elf.txt').write_text(report)  # 保存实际ELF检查输出，便于后续调试查看。
    print('PASS ELF: x86-64 ET_EXEC, no interpreter/dependencies/undefined symbols')  # 只在机器、类型、依赖和符号全部符合后打印通过。

def doctor():  # 查工具与版本，再实际准备和构建内核以验证环境。
    record={}  # 字典以工具名称为键，值保存路径和首行版本。
    for name,formula in [('clang','llvm'),('ld.lld','lld'),('qemu-system-x86_64','qemu'),  # 每项二元组给可执行名称与对应软件包标识。
                         ('xorriso','xorriso'),('gmake','make'),('llvm-readelf','llvm')]:  # 继续列镜像制作、GNU Make和ELF检查器；循环解包到name/formula。
        executable=make_command()[0] if name=='gmake' else tool(name,formula)  # GNU Make沿用gmake再make的选择，其余工具按既有规则查找。
        flag='-version' if name=='xorriso' else '--version'  # xorriso 使用单横线，其余工具使用双横线版本参数。
        result=subprocess.run([executable,flag],capture_output=True,text=True)  # 启动版本命令，分别收集标准输出与错误文本。
        version=(result.stdout+result.stderr).splitlines()[0]  # 版本可能写在任一流，取合并后的第一行。
        record[name]={'path':executable,'version':version}     # 工具键映射到实际路径和版本，不只有安装包名称。
        print(f'OK {name}: {version}')  # 显示本轮查到的工具身份和版本。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    run(make_command(target='kernel'))  # 通过真实内核编译和链接验证环境，不仅看工具是否存在。
    compilation_database()  # 使用同一规则生成编辑器参数。
    inspect_elf()  # 检查刚构建出的普通配置ELF。
    (ROOT/'artifacts/toolchain.json').write_text(json.dumps(record,indent=2)+'\n')  # 保存本次实际工具路径和版本。
def main():  # 解析统一入口动作，并把它交给先前实现的脚本。
    if len(sys.argv)>1 and sys.argv[1]=='learn':  # 先检查有动作参数，再识别采用独立子解析器的learn。
        from learning import main as learn  # 只进入learn时导入后续提供的学习模块，别名避免与当前main混淆。
        learn(sys.argv[2:])  # 切片丢掉程序名和learn，把余下参数交给子入口。
        return  # 当前动作完成，不继续后面的构建或分派。
    parser=argparse.ArgumentParser(description='AiriOS build and learning tools')  # 建立动作解析器，不合法参数由argparse给出用法。
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
    if args.action=='snapshots':  # 只有导出动作需要读取Git教学提交链。
        from snapshots import generate  # 局部导入后续提供的快照模块。
        generate()  # 核对固定main提交链和旧目录保护后导出阶段源码。
        return      # 当前动作完成，不继续后面的构建或分派。
    if args.action=='docs-check':  # 只检查重建与导航时无需启动QEMU。
        from docs_check import check_docs  # 只有明确来源检查才读取全部课程。
        check_docs()  # 核对正文重建与本地导航，不阻塞默认 test。
        return  # 当前动作完成，不继续后面的构建或分派。
    if args.graphics:  # G24已提供纯标准库资产检查，运行图形不依赖G26的截图工具。
        from graphics_assets import verify_fonts  # 显式选择后才读取图形目录。
        verify_fonts()  # 不导入Pillow或Cocoa。
    if args.action in ('ui-preview', 'ui-test') or (args.action == 'test' and args.graphics):  # 完整图形回归在G26后使用。
        from ui_preview import preview, check_frontend  # 主线不会导入这份可选工具。
    if args.action=='ui-preview':  # 主机窗口无需内核启动依赖。
        preview(args.app,args.state)  # 打开指定应用和服务状态。
        return  # 此动作已完成。
    if args.action=='ui-test':  # 主机行为与图像观察采用前章接口。
        check_frontend()  # 运行共享C测试与平台可用快照。
        return  # 此动作已完成。
    if args.action=='clean':  # 清理不需要再构建内核。
        run(make_command(target='clean'))  # 调用Make既有删除规则。
        return  # 清理结束。
    if args.action=='linux-lab':  # Linux对照程序由独立脚本传到个人主机运行。
        run([sys.executable,str(ROOT/'scripts/linux_lab.py'),'--host',args.host,'--phase',args.phase])  # 当前解释器执行个人主机实验。
        return  # 远程实验结束后不启动另一台虚拟机。
    if args.action=='doctor':  # doctor自己按需准备和构建。
        doctor()  # 查工具、构建、生成编辑器参数并检查ELF。
        return    # 环境检查结束。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    if args.graphics:  # 图形和串口使用不同产物目录，不复用旧目标。
        args.profile = 'desktop-' + args.profile  # 如 normal 变为 desktop-normal。
    if args.action=='kernel-debug':  # 内核GDB观察需要先得到普通配置带符号ELF。
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、九种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定九种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 九种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
            work=ROOT/'artifacts'/('interactive-'+args.profile)  # 交互日志按配置分开保存。
            if args.action=='debug':  # debug把CPU停在复位并打开回环GDB端点。
                print('QEMU waits at reset. Remote GDB: target remote localhost:1234\n'  # 告诉调试者当前CPU尚未前进及连接地址。
                      'Then hbreak _start; continue. See dev-log/linux/01-entry.md.',flush=True)  # 提示入口硬件断点，flush保证运行虚拟机前已打印。
            if args.graphics:  # 图形交互仍将UART写入本轮文件。
                print(f'Serial log: {work/"serial.log"}',flush=True)  # 显示本次实际串口日志路径，立即刷新输出。
            cmd = command(args.profile,work,gui=args.graphics,debug=args.action=='debug')  # 先形成具体 QEMU 参数。
            if not args.graphics:  # 串口交互直接连接当前主机终端。
                cmd[cmd.index('-serial')+1] = 'mon:stdio'  # QEMU 用 Ctrl-A X 退出，Ctrl-A C 切监视器。
                print('Serial console: this terminal. Exit QEMU with Ctrl-A X.', flush=True)  # 字节不再写入文件。
            run(cmd)  # 启动所选串口或图形QEMU，debug动作才带-S和GDB。

if __name__=='__main__':  # 直接执行脚本才启动CLI，导入时仅定义接口。
    try:  # 入口错误统一转成明确的失败状态。
        main()  # 执行解析和分派，异常留给下面处理。
    except (RuntimeError,subprocess.CalledProcessError,AssertionError) as error:  # 三类异常都保留具体原因并转成非零退出。
        print(f'ERROR: {error}',file=sys.stderr)  # 错误文字送标准错误，不混入正常结果流。
        sys.exit(1)  # 失败状态交给shell和调用脚本，不能继续宣称通过。
```


```sh
./airios linux-lab --phase p5-threads # 在个人Linux主机编译、运行并取回线程与futex观察。
```

2026-10-16 的既有 SSH 连接实际返回 255，Linux编译、线程执行、内核TID与strace均未验证。Mac只编译环境拒绝分支，运行返回77；Shell语法通过。连接失败见artifacts/linux-p5-threads/connection.log，remote.json明确verified=false。

连接可用时，QUEUE_OK应有tasks=20、sum=210，等待次数按真实调度记录；FUTEX_OK应读value=42，实际等待/唤醒数允许变化。可以在Linux GDB中break consume、run，再用info threads和thread apply all bt观察哪个线程在队列等待，哪个在主流程；既有记录没有实际 GDB 停点证据。

回到BOSS线程池时，先找队列容量、关闭谓词和通知点，再判断等的是pthread内核线程还是运行时任务。bthread由运行时把任务调度到承载线程，任务数量不等于这里的TID数量；具体阻塞边界要看使用的是哪种bthread接口。AiriOS当前没有实现futex、pthread或多核，本实验补足Linux路径，不能用其输出冒充自制内核能力。

依据为[C11委员会草案7.17原子操作](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)、[POSIX条件等待](https://man7.org/linux/man-pages/man3/pthread_cond_wait.3p.html)、[Linux futex](https://man7.org/linux/man-pages/man2/futex.2.html)与[bRPC的bthread说明](https://brpc.apache.org/docs/bthread/bthread/)。
