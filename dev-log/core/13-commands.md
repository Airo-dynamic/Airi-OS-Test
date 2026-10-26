# K13 · 把已有构建、运行与观察收拢为统一入口

构建镜像、启动机器与送入 mem 都已有独立实现。现在让 `./airios run` 依次准备材料、构建并交互运行，让 `./airios test` 组织主机算法、四种启动和输入观察。入口只编排已有动作，不把打印一条总成功文字当成替代验证。

新增 manage.py 与根目录 airios；Makefile、boot.py、console_test.py 继续负责实际工作。doctor 将先检查工具并真正编译、检查 ELF，再由 run 取得终端交互。默认仍是串口目标。

<a id="command-entry"></a>

## 构建参数既给编译器，也给编辑器

辅助 run(cmd) 固定 cwd=ROOT 并等待外部命令，和命令行动作 run 只是同名。build 把 profile 交 Make，normal 还生成 compile_commands.json：每条记录保存 directory、源码绝对 file 与 arguments，供 clangd 按相同目标、头路径与宏分析代码，避免误用主机默认环境。

参数不另抄一份。compilation_database 从 make_command 取 normal/kernel，insert(1,'-Bn') 放到可执行文件之后；-B 强制列全部规则，-n 只打印，收集时不会再执行编译。shlex.split 根据 Shell 引号恢复 argv 边界，找到 -c 后的源路径后写记录。构建规则变化因而同步进入编辑器数据库。

## 链接完成以后，检查是否仍依赖外部运行时

inspect_elf 消费当前 profile 的 ELF，不看另一目录的旧产物。readelf 的 INTERP 说明需要解释器，NEEDED 说明动态库，nm -u 列未定义符号；当前独立内核不能等主机动态链接器来补它们，还必须是 x86-64 ET_EXEC。任一不符都停止，通过后才保存 kernel-elf.txt。文件检查没有执行入口，实际行为仍交启动和输入观察。

**新建 `scripts/manage.py`：**

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

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment')  # 元组固定四种构建配置，CLI choices 与 test 遍历共用。
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

```

## doctor 把工具存在接到真实构建

<a id="toolchain-doctor"></a>

从 00 章版本探测继续：doctor 记录所选工具路径与版本，再 prepare 锁定材料、执行 kernel 构建、生成编译数据库并 inspect_elf。GNU Make 沿用 make_command 第零项，因此仍有 gmake/make 的既定选择，不另建环境规则。

版本可能在 stdout 或 stderr，capture_output 分别收集，text=True 解码，合并取首行放入 record[name]。最后构建与 ELF 检查通过，才写 toolchain.json。该记录说明本次环境实际形成了目标产物，字体与窗口不参与。

**追加到 `scripts/manage.py` 末尾：**

<!-- append: scripts/manage.py -->
```python
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
```

## 一个动作只导入和执行自己需要的组件

<a id="cli-arguments"></a>

`./airios run --profile normal` 经 argparse 成为 Namespace，args.action 为 run、args.profile 为 normal。action 是位置参数，profile 带值，graphics 的 store_true 是开关，省略为 False、出现为 True；choices 在分派前拒绝拼写错误。parse_args 默认从 sys.argv 去掉程序名后读取。

run 先 prepare，再 build，最后形成 QEMU 参数并启动。图形 import 留在实际分支中：图形 run 需 G24 资产校验，完整 ui-test 需 G26 模型与截图工具；默认不打开它们。Linux 在 L01、learn/snapshots 在工具附录才有实现，当前使用已经闭合的 doctor/build/run/test，docs-check 为独立动作。

test 先运行主机检查，再构建四个配置并等启动，最后三次 UART/PS/2；启动与输入结果分别保存在 JSON 数组，任一异常终止。它们各自仍用上章的完成条件，总入口不弱化判断。

交互 run 将 -serial 改为 mon:stdio，普通 mem 字节进入虚拟 UART，结果原路回来；Ctrl-A 后 X 由 QEMU 主机连接识别并退出。自动检查则继续用独立文件/socket。debug 额外 -S 暂停 CPU、开启回环 GDB 端点，后面的 Linux 对照解释停点，不把“等待调试”当启动失败。

**追加到 `scripts/manage.py` 末尾：**

<!-- append: scripts/manage.py -->
```python
def main():  # 解析统一入口动作，并把它交给先前实现的脚本。
    if len(sys.argv)>1 and sys.argv[1]=='learn':  # 先检查有动作参数，再识别采用独立子解析器的learn。
        from learning import main as learn  # 只进入learn时导入后续提供的学习模块，别名避免与当前main混淆。
        learn(sys.argv[2:])  # 切片丢掉程序名和learn，把余下参数交给子入口。
        return  # 当前动作完成，不继续后面的构建或分派。
    parser=argparse.ArgumentParser(description='AiriOS P1 build and learning tools')  # 建立动作解析器，不合法参数由argparse给出用法。
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定四项。
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
        run([sys.executable,str(ROOT/'scripts/linux_lab.py'),'--host',args.host])  # 当前解释器执行个人主机实验。
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
    if args.action=='test':  # 完整验证串联主机算法、前端、四种启动和两种真实输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定四种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 四种启动结果编码为JSON数组。
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

<a id="command-shell"></a>

## Shell 入口只定位工程并转交参数

根目录 airios 先由 `$0` 取得自身路径，dirname 取目录，命令替换 `$(...)` 收集 cd 成功后的 pwd。CDPATH= 仅为该次 cd 清空搜索设置，避免额外输出混入 task_root；`&&` 保证失败时不继续取路径。

exec 用 Python 替换当前 Shell，`"$@"` 保留每个原参数的独立边界。因此 run、--profile、normal 仍是三个字符串，Python 的退出状态也直接回到启动它的终端，没有另一层成功包装。

**新建根目录的 `airios`：**

<!-- write: airios -->
```sh
#!/bin/sh
set -eu  # -e使非零简单命令停止，-u拒绝读取未定义变量。
task_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)  # $0是入口路径；dirname取目录；清空CDPATH避免cd额外输出；&&成功才pwd。
exec python3 "$task_root/scripts/manage.py" "$@"  # 替换shell；双引号$@逐项保留原参数，退出状态直接来自Python。
```

normal 构建已输出编译数据库，再让 clangd 从 build/normal 找它。配置路径相对工程解释，只改变编辑器如何理解代码，不替代构建中的目标选项。

**新建 `.clangd`：**

<!-- write: .clangd -->
```yaml
CompileFlags:  # clangd编译参数设置对象。
  CompilationDatabase: build/normal  # 读取manage导出的普通配置命令数据库。
```

另给格式化工具固定缩进和括号规则，避免日常编辑产生无关外观差异。该文件不参与编译或启动，也不改变 clangd 的参数来源。

**新建 `.clang-format`：**

<!-- write: .clang-format -->
```yaml
BasedOnStyle: LLVM  # 以LLVM风格作为其他未指定项目的默认值。
IndentWidth: 4  # 每层缩进四个空格。
ColumnLimit: 100  # 普通格式化以100列为换行参考。
AllowShortIfStatementsOnASingleLine: Never  # 短if也展开，让状态改变可单独解释。
AllowShortLoopsOnASingleLine: false  # 短循环不挤成同一行。
AllowShortFunctionsOnASingleLine: Empty  # 只有空函数允许单行。
AllowShortBlocksOnASingleLine: Never  # 短块仍展开。
BreakBeforeBraces: Attach  # 左花括号接在声明或条件末尾。
SortIncludes: Never  # 保留作者安排的头文件顺序。
```

构建和观察会留下对象、镜像、日志、Python 缓存与生成的 HISTORY。gitignore 只告诉 Git 默认不收这些未跟踪产物，不删除它们，也不取消已有跟踪。日常源码仍在当前工程修改，阶段副本不能反过来成为实现来源。

**新建 `.gitignore`：**

<!-- write: .gitignore -->
```text
# 编译、镜像和平台程序可由源码重建。
build/
# 运行日志、图片和验证报告不成为源码输入。
artifacts/
# 已校验引导器checkout属于下载依赖。
third_party/limine/
# 下载的压缩包缓存不手敲或跟踪。
third_party/*.tar.gz
# 固定校验的固件镜像保留本地缓存。
third_party/firmware/
# macOS目录显示元数据与实现无关。
.DS_Store
# Python缓存由解释器自动生成。
__pycache__/
# 普通Linux程序编译产物不替代C源码。
labs/linux/p1/ordinary
# 自定义入口程序同样只跟踪汇编源码。
labs/linux/p1/raw
# 远端实验输出可再生，实际证据单独归档。
labs/linux/p1/artifacts/
# 完整快照由固定main导出，不回装进入来源树。
HISTORY/
# 作者技能链接不属于学习者构建输入。
.codex/skills/
# 同一技能的Agent本机入口也不打包。
.agents/skills/
```

当前装载者仍是 Limine。AiriBoot 只有共享合同，尚无 EFI 产物，所以它的 status 可说明边界并成功，build 则必须打印原因、退出 2，让调用方停止。明确失败比生成一个不存在能力的成功标记更符合真实状态。

**新建 `boot/AiriBoot/Makefile`：**

<!-- write: boot/AiriBoot/Makefile -->
```make
# 三个名称都是动作目标，不依赖同名文件是否存在。
.PHONY: help status build
# 默认help执行status，显示当前子项目能力。
help: status
# status只打印当前边界，不制造EFI产物。
status:
	@echo "AiriBoot: shared contract ready; A1/A2/A3 implementation pending." # @隐藏命令回显，文字声明尚未实现。
	@echo "Read README.md. AiriOS P1 currently boots with Limine." # 当前内核仍由固定Limine启动。
# build尚无装载器实现，必须给非零退出而不是假成功。
build:
	@echo "AiriBoot loader is not implemented yet; no EFI output exists." >&2 # >&2把原因送到标准错误。
	@exit 2 # 失败状态交给调用Make的进程。
```

给 shell 入口执行权限，再从工程根目录运行 doctor。它将真正探测、准备、编译并检查文件；工具存在但材料缺失或链接失败，都不能留下完成结论。

**在学习目录根目录运行：**

<!-- run -->
```sh
chmod +x airios  # 为已写出的shell入口添加执行位。
./airios doctor  # 实际校验工具、依赖和独立ELF。
```

doctor 应显示所选版本与 PASS ELF，artifacts 保存路径、版本和 ELF 报告。接着 `./airios run` 在 READY/airi> 后接受 mem，回车再返回本次地图总量，Ctrl-A 后 X 退出；`./airios test` 则自动执行主机检查、四种启动和三次输入观察。

现在自制内核在特权环境直接访问 UART。接下来的 L01 用普通 Linux 程序对照：它如何进入 main，又如何通过系统调用请求输出。随后内核主线从异常报告继续；图形路线仍独立，完成 G24 的 guest 接入后才使用 --graphics。
