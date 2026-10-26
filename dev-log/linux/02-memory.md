# L02 · 在 Linux 分开观察虚拟范围、驻留和访问权限

自制系统已经把物理页、虚拟映射和小对象分开。现在在 Linux 用户程序申请 64 MiB，先只保留范围，再写第一页和所有页，查看哪个变化能反映到驻留统计。随后只读化首个页，让子进程完整执行一次非法写，父进程验证结果后解除范围。

从 K17 继续，新增 p2/memory.c 与 run.sh，再给执行器增加阶段选择；p1 保留。仍用个人 `airi@ssh.airi-dynamic.top` 的 `AiriOS-labs/p2`，不依赖图形。既有记录 SSH255，未运行本版 Linux/GDB，以下是可执行步骤与应观察关系，不预填实测数字，也不据连接问题阻断内核路线。

<a id="linux-vma-rss"></a>

## 地址已经返回，哪些页真正驻留还要继续观察

mmap 交出用户虚拟地址。MAP_PRIVATE|MAP_ANONYMOUS 选择私有匿名范围，不从普通文件 FD 提供内容，因此 fd=−1、offset=0。成功时内核已经保存范围与权限，称为 VMA；不保证每页都有物理页驻留，后续写入才可能触发缺页与按需映射。

snapshot 用地址在 smaps 中找到覆盖它的实际 VMA，打印头、Size、Rss、Private_Dirty、AnonHugePages。这里不是进程总 RSS；相邻同属性范围可能合并，mprotect 也可能拆分，所以不能只看请求长度。特别是首个页改只读后，快照可能只覆盖被拆出的首页，Rss 变小并不说明余下 64 MiB 全被归还。minor_faults 是进程累计量，还含其他访问。

先观察 reserved，再写一页、每页一字节。volatile 保留访问，sysconf 给本机页大小，不把 AiriOS 的 4 KiB 强加给 Linux。可用时 madvise NOHUGEPAGE 减少透明巨大页影响；建议失败仍打印实际巨大页统计，不伪造固定步长或 RSS 数值。

<a id="linux-permission-fault"></a>

## 权限改变后，用子进程承担预期非法写

mprotect 把第一页改为 PROT_READ，更新表和缓存的是 Linux，用户不装 CR3。fork 在父进程返回子 PID，在孩子返回 0，失败为 −1；这里只用它隔离故障，完整地址空间/COW 留到 P3。

孩子写 0x42 应以 SIGSEGV 结束，父进程精确 waitpid 这个孩子，解码 WIFSIGNALED/WTERMSIG 并确认仍读到 0x41。若孩子竟继续，就 `_exit(99)`，父亲拒绝普通退出。fork 前 fflush 避免复制旧 stdio 缓冲后重复打印，RLIMIT_CORE 仅限制本实验与孩子，不改主机策略。

这与 AiriOS 当前内核故障停止全机不同，不能把其精确恢复函数直接当 Linux 信号机制。munmap 之后仅保留地址整数查 proc，不再解引用旧 mapping。

**新建 `labs/linux/p2/memory.c`：**

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

## 父进程验证了孩子的故障，才是这次程序的成功

run.sh 先确认 Linux、将 passed 置 false，再直接运行并保存输出。if 消费真正程序的状态，不经 `程序 | tee` 误取 tee 成功。父进程验证 SIGSEGV 后正常退出 0，GDB 再重新运行一次、在 main 看栈与映射，两个运行都完成才置 true。

程序使用 Linux 的 libc/运行时，没有链接 AiriOS 页表代码。缺 /proc、mprotect 或工具都会保留原因并非零，不能把环境不可用当成预期故障已发生。`$(uname -s)` 取当前内核名，双引号保留一个比较值。

**新建 `labs/linux/p2/run.sh`：**

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

执行器新增 --phase，选 p1/p2 的源码和独立目录。先在本机保存本次 host/directory 与 verified=false，再连 SSH；连接失败也能留下 connection.log/返回码。只有远端运行和取回的 result 都符合才改 true。

传源码、远端编译、运行、打包取证仍是已解释的 SSH/scp/tar 过程。本地语法通过不代替 Linux 的实际 VMA 与信号观察。

**完整替换 `scripts/linux_lab.py`：**

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

统一入口将阶段值交给执行器，不选仍为既有 p1。内核 build/test 不等远端连接，这次实验的未验证状态不会被别的构建成功覆盖。

**完整替换 `scripts/manage.py`：**

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

从统一入口选择 p2，真实结果以该次远端状态和下载文件为准。

<!-- run -->
```sh
./airios linux-lab --phase p2  # 在个人Linux主机真实构建、运行、调试并带回证据。
```

既有记录为 SSH255，remote.json 仍 verified=false、status=connection failed，文字在 connection.log。此前 Mac 仅用 C11/Wall/Wextra/Werror 编译、执行拒绝分支返回 77；没有证明 Linux 驻留、信号或 GDB，本次改稿未重跑。

连接可用后按 memory.log 的 reserved、one page、all pages、read-only、unmapped 依次查看；范围、驻留、权限是不同列，最后应无 VMA 覆盖旧地址。gdb.log 留停点，environment.txt 留版本与页大小，不预写某机器 RSS。手工 GDB 可在各 snapshot 前后对照 smaps；默认孩子分离，父亲仍等实际信号结果。

对应 BOSS/Taishan，申请缓冲成功、已经触碰、数据是否在文件页缓存是不同问题。页分配失败、RSS 增长、O_DIRECT 对齐也不能互相解释；文件缓存与持久性留 P4。参考 [mmap](https://man7.org/linux/man-pages/man2/mmap.2.html)、[smaps](https://man7.org/linux/man-pages/man5/proc_pid_smaps.5.html)、[mprotect](https://man7.org/linux/man-pages/man2/mprotect.2.html)、[fork](https://man7.org/linux/man-pages/man2/fork.2.html)。Linux 按需机制不等于自制系统主动借页映射。
