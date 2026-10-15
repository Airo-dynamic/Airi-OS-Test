# L07 · 把请求交给Linux，再按cookie取回真实完成

从L06继续。epoll告诉应用某个FD现在值得读，io_uring则接收一次具体读操作，稍后把这次操作的结果写进CQ。K32已经实现自己的有界队列，本节用liburing对照Linux SQ/CQ；两套接口不兼容，AiriOS注册缓冲是内核副本，本例Linux缓冲则是整个批次一直存活的普通数组。

<a id="uring-submit"></a>
## SQE保存操作参数，提交数不保证等于准备数

queue_init(8)建立能容纳本批五个请求的环，不使用后台SQ轮询或更改宿主策略。临时文件先完整写入1536个有规律字节；unlink只移除名称，已打开FD继续拥有文件，结束关闭后自动回收。

get_sqe取得尚未提交的SQ项，prep_read写入FD、缓冲地址、长度和文件偏移，set_data64附上100–104这五个整数cookie。前三个读512字节，第四个从1530读16字节，文件只剩6字节；第五个FD=-1，提交可被接受，执行结果应由CQ返回-EBADF。显式偏移读不依赖共享文件位置，不能与普通read的偏移推进混同。

submit_count累计实际返回的提交数量；部分提交继续交剩余项，EINTR在同一两秒预算内重试，零进度明确失败，不能无限空转。liburing函数的负返回值本身是负错误码；代码只为perror设置errno=-rc，不拿别的调用留下的errno解释它。

<a id="uring-completion"></a>
## CQE同时给身份和结果，缓冲区必须活到完成

wait_cqe_timeout最多等两秒；超时是失败，不是一个零字节读。CQE的user_data先解码为缓冲区索引，范围和重复检查通过后再验证对应结果。完成顺序按本轮真实内核记录，可以恰好等于提交顺序；顺序相同不能证明未来也相同，顺序不同也不是程序成功的必要条件。

前三个结果512、第四个6、第五个-EBADF。只有正数结果才有相应有效字节前缀可比较，零表示该读遇到EOF，负数是请求错误。CQ的错误不通过线程errno另行发布。程序复制cookie和result、核对内容之后才cqe_seen，释放这个CQ槽。cqe_seen管理完成槽，不替你释放用户内存；source和buffers是静态数组，整个队列生命期都在，不会把循环栈上的临时数组提前返回。

所有读完成后，再独立准备一个FSYNC，cookie900，等结果0后拆队列和关闭FD。这展示同步也要等完成，但没有目录持久化或物理断电实验。文件初始写入可能仍在页缓存，当前批量读取不是裸盘吞吐测试，也不使用O_DIRECT或固定缓冲注册。

**新建 `labs/linux/p5-uring/uring.c`。** 完整程序包含准备、部分提交、身份核对、错误完成和正常清理。

<!-- write: labs/linux/p5-uring/uring.c -->
```c
#define _GNU_SOURCE // mkstemp等Linux实验接口。
#include <stdio.h> // 真实完成结果输出。
#if defined(__linux__) && defined(__x86_64__) // 只在实际Linux编译liburing路径。
#include <errno.h> // 同步错误与负CQ结果分开。
#include <fcntl.h> // 临时文件和fsync操作。
#include <liburing.h> // SQ/CQ封装，运行时记录实际版本。
#include <stdlib.h> // mkstemp与退出。
#include <string.h> // strerror与数据比较。
#include <time.h> // 部分提交的总截止。
#include <unistd.h> // 写入基线数据与关闭。
static struct io_uring ring; // 从初始化到最后CQ消费一直有效。
static unsigned char source[1536], buffers[5][512]; // 普通缓冲区不移动、不提前复用，不声称已注册固定缓冲。
static void require(int yes, const char *what) { // 每个失败都停止成功路径。
    if (!yes) { perror(what); exit(1); } // 失败退出由OS回收整个进程，正常路径显式消费再释放。
}
static long long now_ms(void) { // 单调时间限制反复提交。
    struct timespec t; require(clock_gettime(CLOCK_MONOTONIC,&t)==0,"clock"); // 不使用墙上时间。
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000; // 转毫秒便于截止比较。
}
static void submit_count(unsigned expected) { // io_uring_submit可能只接受部分SQE。
    unsigned submitted=0; long long end=now_ms()+2000; // 预算不在每次重试时重置。
    while (submitted<expected && now_ms()<end) { // 仅重交尚未接受的SQ尾部。
        int n=io_uring_submit(&ring); // 返回实际提交数，负数是负错误码。
        if (n==-EINTR) continue; // 信号打断可在剩余预算内继续。
        if (n<0) { errno=-n; require(0,"io_uring_submit"); } // 不用可能陈旧的errno解释liburing返回值。
        require(n>0 && (unsigned)n<=expected-submitted,"submission made bounded progress"); // 零进度不忙转。
        submitted+=(unsigned)n; printf("[URING_SUBMIT] accepted=%d cumulative=%u\n",n,submitted); // 记录实际批量结果。
    }
    require(submitted==expected,"submission deadline"); // 未接受完整批次不能假定都在内核。
}
static struct io_uring_cqe *wait_one(void) { // 等待有界，不以队列存在当成功。
    struct __kernel_timespec limit={.tv_sec=2,.tv_nsec=0}; // 单个完成等待最多两秒。
    struct io_uring_cqe *cqe=NULL; int rc=io_uring_wait_cqe_timeout(&ring,&cqe,&limit); // 返回借用CQ槽指针。
    if (rc<0) { errno=-rc; require(0,"CQ wait (timeout is failure)"); } // -ETIME也是明确失败，不是EOF。
    require(cqe!=NULL,"CQ pointer"); return cqe; // 只有成功后才能读结果。
}
int main(void) { // 单线程提交五个读，再独立提交同步请求。
    int rc=io_uring_queue_init(8,&ring,0); // 不使用SQPOLL，不擅自更改系统策略。
    if (rc==-ENOSYS || rc==-EPERM || rc==-EACCES) { // 内核缺能力或策略禁用不能伪装成库测试成功。
        fprintf(stderr,"io_uring unavailable: %s; kernel/policy not changed.\n",strerror(-rc)); return 77; // 如实未验证。
    }
    if (rc<0) { errno=-rc; require(0,"queue_init"); } // 其他初始化错误保留实际原因。
    char path[]="/tmp/airios-uring-XXXXXX"; int fd=mkstemp(path); require(fd>=0,"mkstemp"); // 独立实验文件。
    require(unlink(path)==0,"unlink temporary name"); // FD仍持有文件，退出后不留下路径。
    for (unsigned i=0;i<sizeof source;++i) source[i]=(unsigned char)(i*31+7); // 每个偏移有可核对内容。
    size_t written=0; while (written<sizeof source) { // 写入基线也处理短写。
        ssize_t n=write(fd,source+written,sizeof source-written); // 只提交尚未写入后缀。
        if (n<0 && errno==EINTR) continue; // 尚无进度可重试。
        require(n>0,"seed write"); written+=(size_t)n; // 零或错误都不能算完整文件。
    }
    for (unsigned i=0;i<5;++i) { // SQ容量八，本批五个请求。
        struct io_uring_sqe *sqe=io_uring_get_sqe(&ring); require(sqe!=NULL,"SQ space"); // 空间不足不能写空指针。
        size_t bytes=i==3?16:512; off_t offset=i==3?1530:(off_t)i*512; // EOF请求只剩六字节。
        io_uring_prep_read(sqe,i==4?-1:fd,buffers[i],(unsigned)bytes,offset); // 无效FD也作为请求被提交，错误在CQ返回。
        io_uring_sqe_set_data64(sqe,100+i); // 数字cookie映射到独立缓冲区，不按完成顺序猜身份。
    }
    submit_count(5); unsigned seen=0; // 完成顺序由实际内核决定。
    for (unsigned order=0;order<5;++order) { // 每个普通SQE期望一个CQE。
        struct io_uring_cqe *cqe=wait_one(); // 缓冲区仍然在静态数组中存活。
        uint64_t cookie=io_uring_cqe_get_data64(cqe); int result=cqe->res; // 先保存值再释放CQ槽。
        require(cookie>=100 && cookie<105,"CQ cookie range"); unsigned i=(unsigned)(cookie-100); // 身份决定验证规则。
        require(!(seen&(1u<<i)),"unique CQ cookie"); seen|=1u<<i; // 重复完成不能漏算另一个请求。
        int expected=i==4?-EBADF:(i==3?6:512); require(result==expected,"CQ exact result"); // 负结果直接是-errno。
        if (result>0) require(memcmp(buffers[i],source+(i==3?1530:i*512),(size_t)result)==0,"read bytes"); // 只比较真正读出的前缀。
        printf("[URING_CQ] order=%u cookie=%llu result=%d\n",order,(unsigned long long)cookie,result); // 不要求顺序一定乱序。
        io_uring_cqe_seen(&ring,cqe); // 到这里CQ槽可重用，本程序此后才允许重用对应缓冲。
    }
    struct io_uring_sqe *sqe=io_uring_get_sqe(&ring); require(sqe!=NULL,"fsync SQ"); // 与前面读批次分开，顺序明确。
    io_uring_prep_fsync(sqe,fd,0); io_uring_sqe_set_data64(sqe,900); // flags零表示文件fsync，不代表目录同步。
    submit_count(1); struct io_uring_cqe *cqe=wait_one(); // 同步本身也是有完成结果的请求。
    require(io_uring_cqe_get_data64(cqe)==900 && cqe->res==0,"fsync CQ"); io_uring_cqe_seen(&ring,cqe); // 错误同步不能算落盘成功。
    io_uring_queue_exit(&ring); require(close(fd)==0,"close"); // 全部完成已收取后再拆队列、关闭文件。
    puts("[LINUX_URING_OK] reads=3 short_eof=6 error=-EBADF fsync=0"); return 0; // 只有真实完成全部验证才发布。
}
#else // 非Linux只提供环境拒绝分支。
int main(void) { fputs("x86-64 Linux with liburing is required.\n",stderr); return 77; } // 不能代替Linux编译。
#endif // 完成平台分支。
```

<a id="uring-environment"></a>
## 内核策略可能拒绝建环，失败保持原样

liburing是用户库，安装它不保证内核允许io_uring_setup。ENOSYS、EPERM或EACCES返回77并说明不可用，不自动修改sysctl、容器权限或安全策略。缺开发库时脚本给出Debian/Ubuntu liburing-dev与Arch/CachyOS liburing的包名；不会在个人主机擅自安装。

run.sh保存内核、编译器、liburing和strace实际版本，用strace观察setup/enter/register。普通read SQE不一定需要io_uring_register，日志里没有register也不表示失败。缺工具、初始化拒绝、等待超时和错误CQ都保留false；只有实际程序和所需观察完成，统一执行器才写verified=true。

**新建运行脚本。** pkg-config输出编译和链接参数，实验有三十秒总上限。

<!-- write: labs/linux/p5-uring/run.sh -->
```sh
#!/bin/sh
set -eu # 任一步失败都不发布成功。
mkdir -p artifacts # 即使缺环境也保存明确状态。
printf '{"passed":false,"phase":"p5-uring"}\n' > artifacts/result.json # 撤销旧成功记录。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 固定本轮个人Linux目标。
    echo 'x86-64 Linux with liburing is required.' >&2 # Mac不能提供Linux内核队列。
    exit 77 # 未验证。
fi # 环境检查结束。
for tool in cc pkg-config strace timeout; do # 编译、依赖查询、观察与总运行上界。
    if ! command -v "$tool" >/dev/null 2>&1; then # 缺工具给具体修复提示。
        echo "Missing $tool. Debian/Ubuntu: apt install build-essential pkg-config liburing-dev strace coreutils; Arch/CachyOS: pacman -S base-devel pkgconf liburing strace coreutils." >&2 # 不自动提权安装。
        exit 77 # 保持未验证。
    fi # 当前工具存在。
done # 工具清单完成。
if ! pkg-config --exists liburing; then # 有pkg-config不等于安装了开发库。
    echo 'Missing liburing: Debian/Ubuntu liburing-dev; Arch/CachyOS liburing.' >&2 # 明确包名。
    exit 77 # 不假造CQ。
fi # liburing元数据已找到。
uname -a > artifacts/environment.txt # 真实Linux内核和架构。
cc --version >> artifacts/environment.txt # 实际编译器。
pkg-config --modversion liburing >> artifacts/environment.txt # 真实链接依赖版本。
strace --version >> artifacts/environment.txt # 观察器本身也需可复现。
getconf GNU_LIBC_VERSION >> artifacts/environment.txt 2>&1 || true # 非glibc可没有此项。
cc -std=c11 -O0 -g -Wall -Wextra -Werror uring.c $(pkg-config --cflags --libs liburing) -o uring # 固定工具给出库参数，Shell展开为编译参数。
if timeout -k 2 30 strace -o artifacts/uring-syscalls.log -e trace=io_uring_setup,io_uring_enter,io_uring_register ./uring > artifacts/uring.log 2>&1; then # 三十秒总预算。
    cat artifacts/uring.log # 实际返回与cookie顺序。
else # 依赖策略、超时和完成错误都记录失败。
    status=$?; cat artifacts/uring.log >&2; exit "$status" # 不丢失真实退出码。
fi # 完成实际观察。
grep -q '^\[LINUX_URING_OK\]' artifacts/uring.log # 所有预期CQ都被核对才成功。
printf '{"passed":true,"phase":"p5-uring","registered_buffers":false}\n' > artifacts/result.json # 普通持久数组不冒充固定注册缓冲。
```

**更新阶段映射加入p5-uring。** 仍在个人主机独立目录执行。

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
    'p5-uring': ('uring.c', 'run.sh'), # liburing提交、身份和完成生命周期。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
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

**更新统一入口。** 新阶段不改变guest的编译依赖。

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
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring'),default='p1') # Linux阶段显式选择，默认保持P1。
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
./airios linux-lab --phase p5-uring # 编译实际Linux分支并取回SQ/CQ、错误与版本记录。
```

2026-10-16本轮SSH仍返回255，Linux分支、liburing依赖、内核支持和CQ运行均未验证。Mac仅编译拒绝分支，执行77，脚本语法通过。真实失败保存在artifacts/linux-p5-uring/remote.json与connection.log。不能把正文列出的预期结果当成本轮实际输出。

回看BOSS批量读取时，按请求身份关联目标页、结果长度和错误；检查部分提交后哪些缓冲区已经归内核使用、哪些仍待提交。退出、超时或取消也不自动说明内核再不会访问缓冲区，回收必须依据具体API的完成语义。AiriOS用请求私有副本避免退出后DMA踩用户页，代价是复制与固定容量，这与Linux固定缓冲注册的所有权不同。

依据：[liburing提交手册](https://github.com/axboe/liburing/blob/master/man/io_uring_submit.3)、[有界CQ等待](https://github.com/axboe/liburing/blob/master/man/io_uring_wait_cqe_timeout.3)与[Linux io_uring](https://man7.org/linux/man-pages/man7/io_uring.7.html)。库版本以运行环境文件为准，引用master用于接口说明，不声称已锁定并实际运行该源码版本。
