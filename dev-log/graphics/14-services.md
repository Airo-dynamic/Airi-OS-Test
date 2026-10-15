# G14 · 用同一个内存服务列出、创建和读取文件

画面已经能显示文字，但文件还没有共同来源。现在先不画文件窗口，而是让一个普通 C 程序列出 telemetry 目录，读取 main.lua，再创建 hello.txt 并重新列目录。第二次查询必须看到同一服务中的新记录；重复创建应失败并保留原来的七条记录。

<a id="file-identities"></a>

本章的文件服务运行在普通主机 C 进程中，使用固定数组保存名称与正文，进程退出后内容随之消失。每条记录的 name 是最后一段名称，path 包含父目录，id 是本次服务会话中的数字身份。目录查询把名称、大小等元信息复制到调用者数组，读取文字时再按 id 取得内容。数组下标属于这次查询，排序后位置可以改变，id 才用于再次找到服务记录。

<a id="service-callbacks"></a>

ui_backend 保存服务函数的地址。`bool (*read)(void *, int, char *, size_t)` 中 read 是函数指针，调用者把上下文、文件 id、输出数组和容量交给它。上下文是服务自己的数据入口；这份实现使用静态数组，暂不需要上下文，仍将它原样传回。调用在同一个主机进程中完成。

这里的 `read(context,id,out,cap)` 返回是否复制成功；`files(context,path,out,cap)` 返回复制的记录条数。`action` 的参数依次为上下文、父目录、动作、现有文件 id、新名称、新正文、错误数组及其容量；创建空文件时 id 传 0、正文传空串，成功返回 true，失败才从错误数组读取原因。

**完整写入 `frontend/include/ui.h`：**

<a id="code-frontend-include-ui-h-1"></a>

<!-- write: frontend/include/ui.h -->
```c
#ifndef AIRI_UI_H  // 当前编译单元尚未包含文件服务接口时才展开
#define AIRI_UI_H  // 标记文件服务接口已经包含
#include <airi/boot.h>    // 取得bool、size_t等本次合同所需基础类型
#define UI_DOC_SIZE 8192  // 每份正文最多8191内容字节，另留一格NUL
#define UI_FILES 32  // 参考服务和调用者目录快照各有32个记录槽

enum ui_mode { UI_REFERENCE };  // 本阶段只有主机内存参考服务
// 目录创建是当前唯一修改能力，编辑保存等操作到实际使用时加入。
enum ui_action {  // 目前服务只接受两种创建请求，编号仅在本进程调用中使用
    UI_CREATE_FILE,  // 明确目录中创建空文字文件
    UI_CREATE_DIR    // 明确目录中创建目录项
};
struct ui_file {  // 目录快照记录，只包含身份与元信息，不包含正文
    int id;  // 本服务会话的文件身份，槽号加1，与查询数组下标区分
    bool directory;  // true为目录，不能当普通正文读取
    char name[64];   // 不含父目录的名称，最多63字节及NUL
    char path[128];  // 完整服务路径，最多127字节及NUL
    unsigned bytes;  // 文档内容字节数，不计终止零
};
struct ui_backend {  // 回调表副本由调用者保存，文件记录仍归提供者拥有
    enum ui_mode mode;  // 标明这份数据属于本主机进程的参考服务
    void *context;      // 每次调用原样传回的私有上下文；本提供者使用静态表
    unsigned (*files)(void *, const char *, struct ui_file *, unsigned);  // 目录和数组容量输入，返回实际复制条数
    bool (*read)(void *, int, char *, size_t);  // 按id完整读取正文，容量含NUL，失败不修改输出
    bool (*action)(void *, const char *, enum ui_action, int, const char *, const char *, char *, size_t);  // 显式目录、创建种类与名称输入，失败原因写给调用者
};
void ui_fixture_backend(struct ui_backend *);  // 重置参考服务并填好本阶段三项回调
#endif  // 结束文件服务接口的重复包含保护
```

## 服务保存记录，调用者取得副本

entries 保留固定数量的槽，used 表示槽是否占用。read 只有在正文连NUL能完整放进调用者数组时才复制；容量不足返回false，调用者原数组不改变。id=槽号+1，0 不对应文件。files 只返回当前目录的直接子项；read 把文字复制进调用者缓冲。创建先检查名称、完整路径容量和重名，再占用空槽，错误会通过输出缓冲返回。当前服务按传入的目录字符串拼接路径，没有独立验证父目录是否存在；观察程序始终传初始化中已有的 telemetry 目录。

**完整写入 `frontend/backends/reference.c`：**

<a id="code-frontend-backends-reference-c-1"></a>

<!-- write: frontend/backends/reference.c -->
```c
#include "ui.h"  // 引入当前文件记录和服务回调合同，桌面状态尚未建立
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
/* This provider is linked only into native design tools, never the kernel. */
struct reference_file {  // 内存服务拥有的一条元信息、正文和占用状态
    struct ui_file entry;    // 服务记录内嵌的可复制文件元信息
    char text[UI_DOC_SIZE];  // 服务拥有的文档内容数组
    bool used;  // 服务槽是否占用
};
static struct reference_file entries[UI_FILES];  // 服务拥有32个静态文件槽，跨查询保留而退出进程丢失
static void add(unsigned slot, const char *path, const char *name, bool directory,  // 把路径、名称和文字填进指定内存服务槽
                const char *body) {  // body为新服务记录的NUL正文，目录可传空串
    struct reference_file *f = &entries[slot];  // 借用准备填充的服务槽，add的调用者已选择有效下标
    f->used = true;  // 占用选定服务槽，之后目录查询可见此记录
    f->entry.id = (int)slot + 1;     // 槽号从0开始，服务身份从1开始，0保留为无文件
    f->entry.directory = directory;  // 记录新条目是目录还是普通文字文件
    airi_snprintf(f->entry.name, 64, "%s", name);  // 复制单项名称到服务元信息，64字节数组最多容纳63内容字节与NUL
    airi_snprintf(f->entry.path, 128, "%s/%s", path, name);  // 把父目录、斜杠与单项名称组合成服务完整路径，容量128字节
    airi_snprintf(f->text, sizeof(f->text), "%s", body);  // 复制初始正文到服务拥有的8192字节数组，后续read从此处取得内容
    f->entry.bytes = (unsigned)airi_strlen(body);  // 保存内容长度，不把终止零计入文件字节数
}
static unsigned files(void *unused, const char *path, struct ui_file *out, unsigned cap) {  // 将当前目录直接子项元信息复制给调用者快照
    (void)unused;    // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    unsigned n = 0;  // 本次已复制给调用者的文件条数从0开始，仅此计数之前的out槽有效
    size_t length = airi_strlen(path);  // 当前查询目录的内容字节长度，不含NUL；它决定父路径前缀的比较范围
    for (unsigned i = 0; i < UI_FILES && n < cap; ++i) {  // 逐个检查服务32槽，查询或创建受当前占用状态约束
        struct reference_file *f = &entries[i];  // 借用当前服务槽，检查占用与路径后再复制元信息
        if (!f->used)  // 空槽不参与当前目录查询
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (!airi_memcmp(f->entry.path, path, length) && f->entry.path[length] == '/') {  // 先比较完整父目录前缀，再要求下一字节是斜杠，避免projects误命中projects2
            const char *rest = f->entry.path + length + 1;  // 跳过父目录的length字节和一个斜杠，rest借用子项名称开头
            bool direct = true;    // 先把路径余部视为当前目录直接子项，遇斜杠再否定
            for (; *rest; ++rest)  // 只在父目录之后的路径余部查斜杠，判断是否直接子项
                if (*rest == '/') {  // 子项名称中又有斜杠说明它在更深目录，本次只列直接子项
                    direct = false;  // 路径余部还有层级分隔，条目位于更深目录而非直接子项
                    break;  // 当前路径已经出现子目录分隔符，结束寻找斜杠的循环
                }
            if (direct)  // 仅直接子项进入本次目录查询结果
                out[n++] = f->entry;  // 按旧下标复制整条元信息，再增加返回条数；服务记录未搬动
        }
    }
    return n;  // 返回本次已经复制到调用者数组的有效记录条数
}
static bool read(void *unused, int id, char *out, size_t cap) {  // 按服务id复制完整文档到调用者数组
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    if (id < 1 || id > UI_FILES || !entries[id - 1].used || entries[id - 1].entry.directory)  // 先用短路或保护id-1下标，再要求槽已占用且不是目录，失败不写输出
        return false;  // id无效或指向目录，未复制任何文档内容
    size_t length = airi_strlen(entries[id - 1].text);  // 取得服务正文内容字节数，不含NUL
    if (length >= cap)  // 完整正文及NUL装不下时失败，输出数组保留原内容
        return false;  // 容量不足不能把截断文本当成完整读取成功
    airi_memcpy(out, entries[id - 1].text, length + 1);  // 内容连NUL一起复制，输出由调用者独立拥有
    return true;  // 完整正文连NUL已写入调用者数组，服务自身正文未改变
}
static bool error(char *out, size_t cap, const char *message) {  // 把失败原因写入调用者错误缓冲并返回false
    airi_snprintf(out, cap, "%s", message);  // 将具体失败原因复制到调用者错误数组，cap包含终止零的位置
    return false;  // 失败原因已写出，请求方据false显示错误而不更新保存点
}
static bool valid_name(const char *name) {  // 检查文件名非空、长度与禁止路径分隔符
    size_t n = airi_strlen(name);  // 名称内容字节数用于空名与63字节上限检查，不计NUL
    if (!n || n >= 64 || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.'))  // 拒绝空名、超过63字节以及特殊目录名.和..
        return false;  // 名称含非法形状或字节，阻止后续服务修改
    for (; *name; ++name)  // 逐个验证名称字节，不把终止零当内容
        if (*name == '/' || *name == '\\' || (unsigned char)*name < 32)  // 名称不能含路径分隔符或ASCII控制字节，避免把单个名称当路径
            return false;  // 名称含非法形状或字节，阻止后续服务修改
    return true;  // 所有名称约束均通过，可继续生成目标路径
}
static bool action(void *unused, const char *directory, enum ui_action act, int id,  // 校验请求后改变服务记录，错误留在输出缓冲
                   const char *name, const char *data, char *out, size_t cap) {  // name和data借用请求文字，out与cap由调用者提供来接收本次错误原因
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    (void)id;      // 此阶段只创建新记录，接口保留的现有文件id参数尚不使用
    (void)data;    // 此阶段只创建空记录，接口保留的文字参数尚不使用
    if (act != UI_CREATE_FILE && act != UI_CREATE_DIR)  // 本阶段仅支持创建，其他操作在占用任何服务槽之前报告不可用
        return error(out, cap, "This checkpoint only creates files and directories.");  // 拒绝本次服务修改并向调用者报告：This checkpoint only creates files and directories
    if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
        return error(out, cap, "Use a non-empty file name without slashes, up to 63 bytes.");  // 拒绝本次服务修改并向调用者报告：Use a non-empty file name without slashes, up to 63 bytes
    char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
    int length = airi_snprintf(target, sizeof(target), "%s/%s", directory, name);  // 先拼父目录、斜杠和名称；返回所需字节数供完整路径容量检查
    if (length >= (int)sizeof(target))  // 完整路径所需长度达到128时没有NUL位置，拒绝创建
        return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
    for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
        if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, (size_t)length + 1))  // 只比较已占用槽的完整目标路径，长度加1连NUL一起匹配，重名不覆盖原记录
            return error(out, cap, "An item with that name already exists.");  // 拒绝本次服务修改并向调用者报告：An item with that name already exists
    for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
        if (!entries[i].used) {  // 找到空服务槽后才能新建记录
            add(i, directory, name, act == UI_CREATE_DIR, "");  // 填当前空服务槽，操作种类决定是否目录，初始正文为空
            return true;  // 该服务修改已完整提交，前端可重新查询当前记录
        }
    return error(out, cap, "Reference workspace capacity reached.");  // 拒绝本次服务修改并向调用者报告：Reference workspace capacity reached
}
void ui_fixture_backend(struct ui_backend *b) {  // 重置当前主机参考服务并填入回调表
    airi_memset(entries, 0, sizeof(entries));  // 把服务拥有的32个槽清空，再填本会话初始参考文件
    add(0, "/home/airi/projects/telemetry", "main.lua", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "-- Read a series and write a summary\nlocal values = {12, 18, 21, 32, 29, 37}\n\nlocal "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "sum = 0\nfor _, value in ipairs(values) do\n    sum = sum + value\nend\n\nlocal mean = "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "sum / #values\nprint(string.format(\"mean: %.2f\", mean))\n\n-- Run requires the future "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "application service.\n-- This reference workspace only edits the source.\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(1, "/home/airi/projects/telemetry", "series.csv", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "seconds,load\n0,12\n1,18\n2,21\n3,32\n4,29\n5,37\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(2, "/home/airi/projects/telemetry", "README.md", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "# Telemetry workspace\n\nThis is a frontend reference dataset.\nSave changes update this "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "session's memory only.\nNative application execution is intentionally unconnected.\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(3, "/home/airi/projects/telemetry", "output", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(6, "/home/airi/projects/telemetry", "sum.c", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "#include <stdio.h>\n\nint main(void) {\n    const int values[] = {12, 18, 21, 32};\n    "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "int total = 0;\n    for (unsigned i = 0; i < 4; ++i)\n        total += values[i];\n    "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "printf(\"sum: %d\\n\", total);\n    return 0;\n}\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(7, "/home/airi/projects/telemetry", "project.airi", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "[project]\nname = telemetry\nentry = main.lua\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(4, "/home/airi/projects", "telemetry", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(5, "/home/airi", "projects", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    *b = (struct ui_backend){.mode = UI_REFERENCE, .files = files, .read = read, .action = action};  // 通过b写调用者的整份回调表，函数地址交给files/read/action，未指定context字段自动为NULL
}
```

## 连续观察读取与创建

观察程序先取得六条元信息，按第一项的 id 读取 main.lua，然后创建并重新查询。调用返回的条数决定 snapshot 的有效前缀；旧数组不是服务本身，只有再次查询才能获得新记录。

**完整写入 `labs/tutorial/provider.c`：**

<a id="code-labs-tutorial-provider-c-1"></a>

<!-- write: labs/tutorial/provider.c -->
```c
#include "ui.h"     // 引入当前文件记录和服务回调合同，桌面状态尚未建立
#include <stdio.h>  // 主机观察使用标准文件输出与打印接口
#include <airi/base.h>  // 用自己的字节比较核对失败读取没有改动输出数组
int main(void) {  // 在主机准备输入、运行当前观察并以退出码报告结果
    const char *directory = "/home/airi/projects/telemetry";  // 本次查询与创建使用同一明确父目录，不靠查询改变服务隐式状态
    struct ui_backend backend;  // 观察程序持有回调合同副本，初始化后才可调用
    struct ui_file snapshot[UI_FILES];   // 调用者拥有32槽元信息数组，条数决定有效前缀
    char text[UI_DOC_SIZE], error[160];  // text接收main.lua正文；error接收创建失败原因，都是调用者独立数组
    ui_fixture_backend(&backend);  // 重置主机内存文件记录，把读写函数填入本观察程序的 backend 回调表
    unsigned before = backend.files(backend.context, directory, snapshot, UI_FILES);  // 将当前目录直接子项元信息复制给调用者快照
    if (before != 6)  // 固定目录应有六条；先确认有效条数，再访问 snapshot[0]
        return 1;  // 目录状态不符时停止，避免读取无有效记录的数组
    if (!backend.read(backend.context, snapshot[0].id, text, sizeof(text)))  // 用查询第0项保存的服务id读取正文，失败时结束观察，不能把快照下标当id
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    printf("files=%u first=%s bytes=%u\n", before, snapshot[0].name, snapshot[0].bytes);  // 向主机终端报告本次观察的实际状态或输出位置
    char small[4] = "xyz";  // 只能容纳三内容字节及NUL，装不下本服务的main.lua全文
    bool copied = backend.read(backend.context, snapshot[0].id, small, sizeof(small));  // 故意提供不足容量，服务应拒绝整次复制
    printf("small buffer: success=%d preserved=%s\n", copied, small);  // 同时观察失败返回值和调用者原字节
    if (copied || airi_memcmp(small, "xyz", sizeof(small)))  // 任一成功或四字节变化都违反完整读取的合同
        return 1;  // 拒绝把截断或部分写入报告为通过
    bool created = backend.action(backend.context, directory, UI_CREATE_FILE, 0,  // 校验请求后改变服务记录，错误留在输出缓冲
                                  "hello.txt", "", error, sizeof(error));  // 明确创建名称hello.txt，正文为空；错误原因只写160字节调用者数组
    unsigned after = backend.files(backend.context, directory, snapshot, UI_FILES);  // 将当前目录直接子项元信息复制给调用者快照
    printf("after create: files=%u success=%d\n", after, created);  // 向主机终端报告本次观察的实际状态或输出位置
    bool duplicate = backend.action(backend.context, directory, UI_CREATE_FILE, 0,  // 校验请求后改变服务记录，错误留在输出缓冲
                                    "hello.txt", "", error, sizeof(error));  // 明确创建名称hello.txt，正文为空；错误原因只写160字节调用者数组
    printf("duplicate: success=%d error=%s\n", duplicate, error);  // 向主机终端报告本次观察的实际状态或输出位置
    return before != 6 || after != 7 || !created || duplicate;     // 初始六条、新建后七条、创建成功且重名失败才以0退出
}
```

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 创建本学习目录的产物目录，已有目录可复用
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/provider.c frontend/backends/reference.c kernel/lib/base.c -o build/probe  # 按当前检查点源码链接主机观察程序，-I提供接口目录，-o指定输出文件
./build/probe  # 运行刚构建的本检查点观察程序，退出码和输出共同核对结果
```

第一次查询得到六条，正常数组读到 main.lua。容量只有四字节的读取输出 `small buffer: success=0 preserved=xyz`：服务先比较正文长度和容量，失败返回前没有写 small。创建后得到七条，重复创建返回 false 和重名原因。退出进程后这些记录消失；重跑会重新初始化。接下来把查询副本放进桌面状态，画出窗口。
