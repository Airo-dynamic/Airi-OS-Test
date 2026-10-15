# L02 · 在 Linux 分开观察虚拟范围、驻留和访问权限

从 K17 继续，沿用个人 Linux 主机，不需要图形课程。AiriOS 已经亲手实现物理页、页表和对象堆，现在换到 Linux 用户程序，申请 64 MiB 虚拟范围，逐步写入，再把第一页改成只读，让一个独立子进程完整演示非法写入的结果。最后解除范围，记录真实 VMA、RSS 和缺页次数。

本节新建 labs/linux/p2/memory.c 与 run.sh，再替换 Linux 执行脚本和统一入口，旧 P1 程序不变。默认 Linux 主机仍是 airi@ssh.airi-dynamic.top，新的独立远端目录是 AiriOS-labs/p2。本轮 SSH 连接返回255，尚未运行 Linux 程序或 GDB；以下给出完整操作与应观察的关系，不把示意关系当作实测数字。连接失败不要求暂停内核主线。

<a id="linux-vma-rss"></a>
## mmap 返回了什么

mmap 返回一个用户虚拟地址。MAP_PRIVATE|MAP_ANONYMOUS 表示私有匿名范围，不从某个普通文件 FD 读取初始内容；fd 参数因此为−1，文件偏移为0。成功后 Linux 已经记住该范围和权限，这段管理记录称为 VMA。它不保证每一页此时都已经分配并驻留在物理内存中；真实写入可能触发缺页，再建立本次访问需要的物理页和映射。

RSS 是驻留集大小，本节读取 /proc/self/smaps 中包含目标地址的那一段 VMA 的 Rss，而不是把进程总内存变化全部算到这次 mmap。映射头给出实际起止和权限；内核可能把相邻同属性区域合并，mprotect 又可能将一个 VMA 拆成多个，因此同时输出 Size 和头行。Private_Dirty 是该 VMA 私有脏页统计，AnonHugePages 记录匿名透明巨大页；它们解释为什么观察值不一定以预想粒度变化。进程累计 minor_faults 也包含其他访问，不能把它精确当成这一个 VMA 的缺页数。

程序先只建立范围，再写第一页，最后每页写一个字节。volatile 保证这些实际访问不会被编译器删除。页大小用 sysconf 查询，不能把 AiriOS 当前4KiB假设带到任意 Linux。若内核支持，madvise NOHUGEPAGE 减少透明巨大页影响；建议失败会保留具体日志并继续输出真实巨大页统计，不伪造固定RSS结果。

<a id="linux-permission-fault"></a>
## 让非法写影响一个进程

mprotect 把首个实际页改成 PROT_READ，Linux 负责更新页表权限与相关翻译缓存，用户程序不能自己装 CR3。随后 fork 创建子进程：同一调用在父进程返回子 PID，在子进程返回0，失败返回−1。本节只用它隔离一次明确的非法写；完整进程地址空间、复制与 COW 在 P3 继续研究。

子进程写只读页应被 Linux 以 SIGSEGV 终止；父进程不做这次写，用 waitpid 等刚创建的子进程，再用 WIFSIGNALED/WTERMSIG 解码状态。父进程仍能读原值0x41。AiriOS 目前没有用户进程，未登记故障会停止内核；Linux 将这次非法用户访问限制到子进程。这是两套运行环境的真实差别，不能把 AiriOS 的内核 PF 恢复代码直接当 Linux 信号处理。

fork 前 fflush 清掉已有 stdio 缓冲，避免复制缓冲后重复输出。子进程若没有发生预期故障，则执行 _exit(99)，不经过复制来的 stdio 清理；父进程会拒绝这个普通退出。RLIMIT_CORE 只限制当前实验进程及其孩子，不修改系统配置，不产生预期故障的 core 文件。

**新建 `labs/linux/p2/memory.c`。** snapshot 只比较地址数值和 proc 头；munmap 后不再解引用旧指针。

<!-- write: labs/linux/p2/memory.c -->
```c
#define _GNU_SOURCE // Linux提供匿名mmap、proc和当前实验用的POSIX扩展声明。
#include <errno.h> // 失败后立即读取errno的文字表示。
#include <signal.h> // SIGSEGV用于识别只读页写入的进程结果。
#include <stdint.h> // uintptr_t承接指针数值。
#include <stdio.h> // 读取proc并输出实验结果。
#include <stdlib.h> // EXIT_FAILURE和exit。
#include <string.h> // 只筛选当前VMA的几个字段。
#include <sys/mman.h> // mmap、mprotect、munmap。
#include <sys/resource.h> // 禁止core文件并观察minor fault计数。
#include <sys/wait.h> // 父进程等待故障子进程。
#include <unistd.h> // 页大小、PID和fork。
static void fail(const char *operation) { // 保留具体失败系统接口名。
    perror(operation); // 当前errno在做其他系统操作前写到stderr。
    exit(EXIT_FAILURE); // 失败不继续运行后续观察。
}
static int snapshot(const char *stage, uintptr_t address) { // 只读取包含这个地址的VMA统计。
    struct rusage usage; // 观察本进程累计缺页，不把它解释成某个VMA独占数据。
    if (getrusage(RUSAGE_SELF, &usage) != 0) // 系统调用失败需要明确报告。
        fail("getrusage"); // 不输出未初始化计数。
    printf("\n[%s] pid=%ld address=0x%lx minor_faults=%ld\n", stage, (long)getpid(), (unsigned long)address, usage.ru_minflt); // 计数为进程累计值。
    FILE *maps = fopen("/proc/self/smaps", "r"); // Linux内核提供本进程映射与驻留统计。
    if (!maps) // proc未挂载或无权限时不能伪造RSS。
        fail("open /proc/self/smaps"); // 明确环境限制。
    char line[512]; // 一行足以读取映射头与本实验选择的统计字段。
    int inside = 0, found = 0; // 当前行是否属于目标VMA，以及是否找到过它。
    while (fgets(line, sizeof(line), maps)) { // 逐行消费同一次proc快照读取。
        unsigned long long start, end; // Linux映射头中的十六进制半开范围。
        char permissions[5]; // 四个权限字符加终止零。
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3) { // 新映射头开始，重新判断所属VMA。
            inside = address >= start && address < end; // 目标地址只属于包含它的一段。
            if (inside) { // 输出实际映射范围和权限，可能被内核和邻居合并。
                found = 1; // 后面可判断解除后是否仍有映射。
                fputs(line, stdout); // 保留真实头，不按请求长度编造VMA。
            }
        } else if (inside && (!strncmp(line, "Size:", 5) || !strncmp(line, "Rss:", 4) || !strncmp(line, "Private_Dirty:", 14) || !strncmp(line, "AnonHugePages:", 14))) // 只选择当前机制相关字段。
            fputs(line, stdout); // RSS和巨大页数都是本次实际内核统计。
    }
    if (ferror(maps)) // EOF和读取失败必须区分。
        fail("read smaps"); // 不把截断快照当完成。
    fclose(maps); // 结束对proc文件的借用。
    if (!found) // munmap后预期没有任何VMA覆盖旧地址。
        puts("no VMA contains this address"); // 不解引用已释放指针，只比较地址数值。
    return found; // 主流程检查映射存在与解除的结果。
}
int main(void) { // 普通Linux用户态程序，由Linux拥有页表和物理页。
#ifndef __linux__ // 源码可在Mac检查语法，运行明确拒绝冒充Linux。
    fputs("This experiment requires Linux /proc and Linux VM semantics.\n", stderr); // 给出环境原因。
    return 77; // 77表示未在所需环境运行，不是实验成功。
#endif // Linux继续实际观察。
    struct rlimit no_core = {0, 0}; // 只读写入的预期信号不生成core文件。
    if (setrlimit(RLIMIT_CORE, &no_core) != 0) // 禁止修改无关主机配置，仅限制本进程及其子进程。
        fail("setrlimit core"); // 设置失败也保留原因。
    long page = sysconf(_SC_PAGESIZE); // 不假定Linux目标也一定使用4KiB页。
    if (page <= 0) // 查询失败不能作为循环步长。
        fail("sysconf page size"); // 避免无穷循环。
    size_t bytes = 64u * 1024u * 1024u; // 申请64MiB匿名虚拟范围，不等于立刻占用64MiB物理RAM。
    volatile unsigned char *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // -1表示不由文件FD提供内容，偏移为零。
    if (mapping == MAP_FAILED) // mmap失败值是(void*)-1，不是NULL。
        fail("mmap"); // 不访问失败哨兵。
    uintptr_t address = (uintptr_t)mapping; // 保存数值用于后续proc范围查询。
    printf("page_bytes=%ld requested=%zu\n", page, bytes); // 记录当前环境而非写死页大小。
#ifdef MADV_NOHUGEPAGE // Linux支持时减少透明巨大页对RSS步进的影响。
    if (madvise((void *)mapping, bytes, MADV_NOHUGEPAGE) != 0) // 建议失败不改变mmap本身的成功。
        perror("madvise NOHUGEPAGE (continuing)"); // 后面仍打印AnonHugePages说明实际环境。
#endif // 不支持时也可以观察，不宣称固定粒度。
    if (!snapshot("reserved, not touched", address)) // 地址范围必须已经出现在映射图。
        fail("new VMA not found"); // 不用进程总RSS代替映射存在性。
    mapping[0] = 0x41; // 第一次真实写入，Linux为该范围处理所需缺页。
    snapshot("one page written", address); // 对比该VMA实际RSS和进程minor fault计数。
    for (size_t i = 0; i < bytes; i += (size_t)page) // 每页至少写一个字节，不只reserve地址。
        mapping[i] = 0x41; // volatile保证编译器保留这些实际访问。
    snapshot("all pages written", address); // 不强制RSS精确等于请求量，保留实际结果。
    if (mprotect((void *)mapping, (size_t)page, PROT_READ) != 0) // 首个页改成只读，Linux更新页表和缓存。
        fail("mprotect read-only"); // 权限设置失败不能继续预期故障。
    snapshot("first page read-only", address); // VMA可能被分成只读首段和后续读写段。
    fflush(NULL); // fork前刷完stdio缓冲，避免子进程复制旧输出后重复打印。
    pid_t child = fork(); // 返回0的是子进程；父进程得到其PID；失败为-1。
    if (child < 0) // 失败没有子进程可以等待。
        fail("fork fault child"); // 不继续访问未知PID。
    if (child == 0) { // 只让子进程执行预期非法写，父进程负责记录结果。
        mapping[0] = 0x42; // 只读页写入应被Linux转为SIGSEGV。
        _exit(99); // 若错误地返回这里，父进程会把它判为失败。
    }
    int status; // waitpid写入编码后的退出或信号状态。
    if (waitpid(child, &status, 0) != child) // 精确等待刚创建的那个子进程。
        fail("waitpid"); // 不把未取得状态当成正常退出。
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV || mapping[0] != 0x41) { // 要求实际信号终止且父进程仍可读取原值。
        fputs("read-only fault did not produce the expected SIGSEGV\n", stderr); // 明确哪项机制未成立。
        return EXIT_FAILURE; // 不输出完成标记。
    }
    printf("fault_child=%ld signal=%d parent_value=0x%x\n", (long)child, WTERMSIG(status), mapping[0]); // Linux将用户访问故障限制在进程内。
    if (munmap((void *)mapping, bytes) != 0) // 结束整个范围的借用，包括被mprotect拆分的VMA。
        fail("munmap"); // 失败不能声称映射已解除。
    if (snapshot("unmapped", address)) // 这里只比较旧地址数值，绝不再次访存。
        return EXIT_FAILURE; // 旧VMA仍存在则拒绝完成。
    puts("[LINUX_P2_OK] reserve-touch-protect-fault-unmap"); // 所有实际步骤完成才发布标记。
    return 0; // 正常返回运行时退出。
}
```

<a id="linux-run-record"></a>
## 让失败结果也留下记录

run.sh 首先要求当前内核确实为 Linux，再把 result.json 写成 passed=false，避免旧成功在新失败后继续出现。普通程序的输出直接写文件，if 使用真实退出状态；不用 `程序 | tee` 后误把 tee 的成功当程序成功。GDB 在 main 停下，输出调用栈和真实映射，再继续完整实验。两个程序运行都完成后才发布 passed=true。

本节程序由 Linux 的 libc 和运行时进入 main；AiriOS 的字节库和页表代码没有链接进去。故障子进程的 SIGSEGV 是实验结果的一部分，父进程验证它后正常退出0。若 /proc、mprotect 或工具不可用，脚本保留失败输出并非零退出。

**新建 `labs/linux/p2/run.sh`。** `$(uname -s)` 只取得当前内核名称，双引号保留它作为单个测试值。

<!-- write: labs/linux/p2/run.sh -->
```sh
#!/bin/sh
set -eu # 任一步失败即停止，未设置变量也失败。
if [ "$(uname -s)" != Linux ]; then # 当前内核必须是真正Linux。
    echo 'Linux is required; host execution is not a Linux validation.' >&2 # 不把Mac运行当对照证据。
    exit 77 # 明确未验证。
fi # 完成环境检查。
mkdir -p artifacts # 保存本次环境、输出和GDB记录。
printf '{"passed":false,"phase":"p2"}\n' > artifacts/result.json # 先覆盖旧成功，后续失败不能误用旧结果。
uname -a > artifacts/environment.txt # 记录实际内核和机器架构。
getconf PAGE_SIZE >> artifacts/environment.txt # 记录Linux实际页大小。
cc --version >> artifacts/environment.txt # 记录本轮宿主C编译器。
cc -std=c11 -O0 -g -Wall -Wextra -Werror memory.c -o memory # 普通libc程序，不使用AiriOS对象。
if ./memory > artifacts/memory.log 2>&1; then # 保存真实运行退出状态，不经pipeline丢失它。
    cat artifacts/memory.log # 将已完成输出交回调用者。
else # 程序失败也保留本轮输出。
    status=$? # 第一时间取得失败程序退出码。
    cat artifacts/memory.log >&2 # 回显具体失败位置。
    exit "$status" # 保留实际失败状态。
fi # 程序已确认映射和SIGSEGV路径。
command -v gdb > artifacts/gdb.log # 缺调试器时明确失败，不能跳过后宣称全通过。
gdb -q -batch -ex 'set pagination off' -ex 'break main' -ex run -ex 'bt' -ex 'info proc mappings' -ex continue ./memory >> artifacts/gdb.log 2>&1 # 真正停在main再运行完整实验。
printf '{"passed":true,"phase":"p2"}\n' > artifacts/result.json # 程序与调试器都正常结束后才发布成功。
```

Linux 执行器新增 --phase，按 p1/p2 选本节源文件和独立远端目录。实际建立 SSH 连接前，先在本机写 verified=false 和本次 host/directory；连接失败仍保存 connection.log 和返回码。只有远端执行及下载的 result.json 都明确成功，才改为 verified=true。源码复制、远端运行和回收证据仍通过已解释的 SSH、scp、tar；实验没有进入远端时，不拿本地编译替代它。

**完整替换 `scripts/linux_lab.py`。** 失败证据在复制源码之前就能落到本机，不会只留一条终端文字。

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

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
parser.add_argument('--phase', choices=('p1', 'p2'), default='p1') # 实验来源与远端目录使用同一阶段名。
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
names = ('ordinary.c','raw_start.S','run.sh') if args.phase == 'p1' else ('memory.c','run.sh') # 只选择本阶段已写出的程序。
sources=[ROOT/'labs/linux'/args.phase/name for name in names]  # 列表推导按所选阶段名称构成Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True)  # 列表内*展开本阶段路径字符串，最后一项是远端目标。
execution=subprocess.run(ssh+[f'cd {shlex.quote(remote)} && sh run.sh'])  # 远端先切实验目录，再执行shell；暂不check，失败也先下载证据。
data=subprocess.check_output(ssh+[f'cd {shlex.quote(remote)} && tar -cf - artifacts'])  # tar从标准输出交回归档bytes，-f -表示不写远端归档文件。
dest.mkdir(parents=True,exist_ok=True)  # 确保证据父目录存在。
with tarfile.open(fileobj=io.BytesIO(data)) as tar:  # 把内存归档作为文件交给tarfile，with结束关闭归档。
    tar.extractall(dest,filter='data')  # data过滤限制危险成员，解包到本轮证据目录。
if execution.returncode:  # 非零远端状态意味着实验失败，即使tar传输成功。
    raise RuntimeError(f'Linux experiment failed; evidence downloaded to {dest}')  # 原始证据已经带回，再报告失败位置。
result=json.loads((dest/'artifacts/result.json').read_text())  # 实际远端程序写出的JSON恢复成字典。
if result.get('passed') is not True:  # get缺键返回None，只有布尔True才接受。
    raise RuntimeError('Remote validation failed')  # 远端记录不满足成功合同。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':True,'status':'program and GDB completed'},indent=2)+'\n')  # 记录本次实际host和目录，不把另一主机记录混用。
print(f'Remote evidence downloaded: {dest}')  # 显示本轮证据可查看位置。
```

**完整替换 `scripts/manage.py`。** 新参数转交 Linux 执行器；不选择阶段仍运行既有 P1，内核构建不依赖远端。

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
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal')  # 元组固定五种构建配置，CLI choices 与 test 遍历共用。
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
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定五项。
    parser.add_argument('--phase',choices=('p1','p2'),default='p1') # Linux阶段显式选择，默认保持P1。
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
    if args.action=='test':  # 完整验证串联主机算法、可选前端、五种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定五种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 五种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
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

<!-- run -->
```sh
./airios linux-lab --phase p2  # 在个人Linux主机真实构建、运行、调试并带回证据。
```

本轮实际结果是 SSH 返回255，`artifacts/linux-p2/remote.json` 中 verified=false、status=connection failed，具体连接文字在同目录 connection.log。C 源码已在 Mac 以 C11、Wall/Wextra/Werror 编译通过；Mac 运行明确返回77并说明需要 Linux，这只验证语法与错误环境出口。远端的保留范围、触碰后 RSS、SIGSEGV 和 GDB 停点均尚未验证。

连接可用后的操作：在下载的 `artifacts/linux-p2/artifacts/memory.log` 顺序查看 reserved、one page、all pages、read-only、unmapped 五个快照。前四段展示范围、驻留和权限变化，最后应找不到覆盖旧地址的 VMA；不预填某台机器的 RSS 数字。GDB 输出在 gdb.log，环境、编译器和页大小在 environment.txt。单独登录该目录也可以执行 `gdb ./memory`、`break main`、`run`、`next`，在每个 snapshot 前后对照 smaps；子进程默认从 GDB 分离，父进程等待它的真实信号结果。

回到 BOSS/Taishan：申请缓冲区成功、已经触碰对应页、数据是否在页缓存中，是不同问题。页分配失败、进程 RSS 增长和 O_DIRECT 的对齐要求也不能用同一个统计解释。本节覆盖虚拟范围、实际触碰和权限；文件页缓存、直接I/O和持久性在 P4 的专门实验处理。

参考：[mmap](https://man7.org/linux/man-pages/man2/mmap.2.html)、[smaps](https://man7.org/linux/man-pages/man5/proc_pid_smaps.5.html)、[mprotect](https://man7.org/linux/man-pages/man2/mprotect.2.html)、[fork](https://man7.org/linux/man-pages/man2/fork.2.html)。Linux 的按需分配比当前 AiriOS 主动借页并映射更完整，不能据本节把两者当成同一种内存策略。
