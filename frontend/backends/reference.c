#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
/* This provider is linked only into native design tools, never the kernel. */
struct reference_file {  // 内存服务拥有的一条元信息、正文和占用状态
    struct ui_file entry;    // 服务记录内嵌的可复制文件元信息
    char text[UI_DOC_SIZE];  // 服务拥有的文档内容数组
    bool used;  // 服务槽是否占用
};
static struct reference_file entries[UI_FILES];  // 服务拥有32个静态文件槽，跨查询保留而退出进程丢失
static AiriBootInfo boot;  // 参考服务持有示例启动结构，借给System与console使用
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
    for (unsigned i = 0; i < UI_FILES && n < cap; ++i) {  // 遍历最多32个服务槽，输出有效条数达到cap时停止复制
        struct reference_file *f = &entries[i];  // 借用当前服务槽，检查占用与路径后再复制元信息
        if (!f->used)  // 空槽不参与当前目录查询
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (!airi_memcmp(f->entry.path, path, length) && f->entry.path[length] == '/') {  // 先比较完整父目录前缀，再要求下一字节是斜杠，避免projects误命中projects2
            const char *rest = f->entry.path + length + 1;  // 跳过父目录的length字节和一个斜杠，rest借用子项名称开头
            bool direct = true;    // 先把路径余部视为当前目录直接子项，遇斜杠再否定
            for (; *rest; ++rest)  // 只在父目录之后的路径余部查斜杠，判断是否直接子项
                if (*rest == '/') {  // 子项名称中又有斜杠说明它在更深目录，本次只列直接子项
                    direct = false;  // 路径余部还有层级分隔，条目位于更深目录而非直接子项
                    break;  // 此路径含子目录分隔符，结束当前斜杠查找循环
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
    return false;  // 失败原因已写到调用者数组，false表示服务记录没有提交本次修改
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
    add(5, "/home/airi", "projects", true, "");      // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    boot = (AiriBootInfo){.magic = AIRI_BOOT_MAGIC,  // 为主机窗口准备固定参考启动结构，System明确标成示例数据
                          .version = AIRI_BOOT_VERSION,  // 设置启动合同版本
                          .size = sizeof(boot),  // 设置本版启动结构字节数
                          .direct_map_offset = UINT64_C(0xffff800000000000),  // 设置参考直接映射偏移
                          .kernel_physical_base = 0x100000,  // 设置参考内核物理起点
                          .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 设置参考内核虚拟起点
                          .kernel_size = 132 * 1024,  // 设置参考内核镜像长度
                          .region_count = 4};  // 设置参考内存区域有效条数
    airi_snprintf(boot.loader, 32, "Reference / UEFI");  // 写明这份启动结构属于主机参考数据，System据此区别实际guest启动
    boot.regions[0] =  // 参考地图第0项描述最低1MiB保留范围，下一行填完整记录
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段名给起点0、长度1MiB和保留类别
    boot.regions[1] = (struct airi_memory_region){  // 参考地图第1项填写内核占用范围
        .base = 0x100000, .length = 0x100000, .kind = AIRI_MEMORY_KERNEL};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    boot.regions[2] = (struct airi_memory_region){  // 参考地图第2项填写当前保留的引导可回收范围
        .base = 0x200000, .length = 0x2900000, .kind = AIRI_MEMORY_BOOT_RECLAIM};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    boot.regions[3] = (struct airi_memory_region){  // 参考地图第3项填写可用物理范围
        .base = 0x2b00000, .length = 0xd500000, .kind = AIRI_MEMORY_USABLE};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    *b = (struct ui_backend){.mode = UI_REFERENCE, .boot = &boot, .files = files, .read = read, .action = action};  // 通过b写调用者的整份回调表，函数地址交给files/read/action，未指定context字段自动为NULL
}
