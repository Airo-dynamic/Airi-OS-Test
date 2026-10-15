#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
/* This provider is linked only into native design tools, never the kernel. */
struct reference_file {  // 内存服务拥有的一条元信息、正文和占用状态
    struct ui_file entry;    // 服务记录内嵌的可复制文件元信息
    char text[UI_DOC_SIZE];  // 服务拥有的文档内容数组
    bool used;  // 服务槽是否占用
};
static struct reference_file entries[UI_FILES];  // 服务拥有32个静态文件槽，跨查询保留而退出进程丢失
static struct ui_task records[UI_TASKS];  // 服务拥有最多16条静态参考任务记录
static unsigned record_count;  // 保存静态任务数组的有效前缀长度
static AiriBootInfo boot;      // 参考服务持有示例启动结构，借给System与console使用
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
                    break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
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
static bool action(void *unused, const char *directory, enum ui_action act, int id, const char *name, const char *data,  // 校验请求后改变服务记录，错误留在输出缓冲
                   char *out, size_t cap) {  // 错误正文写到调用者out数组，cap是该数组的字节容量，服务不保留此地址
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    if (act == UI_EXECUTE)  // 执行请求暂不启动主机或guest程序，返回真实未连接错误
        return error(out, cap,  // 把失败原因写给调用者，false阻止前端误记为成功
                     "A program runtime is not connected. Your source is still open.");  // 执行失败只说明用户当前可做的事，不把开发计划放进错误提示

    if (act == UI_STOP) {  // 停止请求处理参考任务表，不向主机发送终止信号
        if (id <= 2)  // 参考桌面与终端记录受保护，不能从样例表移除
            return error(out, cap, "This system task is protected.");  // 拒绝本次服务修改并向调用者报告：The reference desktop and terminal records are protected
        for (unsigned i = 0; i < record_count; ++i)  // 只检查服务当前有效任务记录，未用槽不参与
            if (records[i].pid == id) {  // 找到所请求的参考任务身份后才修改其状态，其他记录不受影响
                airi_memmove(records + i, records + i + 1,  // 搬移可能重叠的当前有效前缀或尾部，复制长度由当前对象内容或容量计算
                             (record_count - i - 1) * sizeof(*records));  // 只前移被删项之后的有效任务，条数乘每项大小得到字节数
                --record_count;  // 任务表后缀已前移，有效任务数量减少一个
                return true;     // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "The selected process record no longer exists.");  // 拒绝本次服务修改并向调用者报告：The selected process record no longer exists
    }
    if (act == UI_CREATE_FILE || act == UI_CREATE_DIR || (act == UI_SAVE && id < 0)) {  // 创建项目和未绑定id的另存都需要新槽；已存在文档的保存后面按id处理
        if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
            return error(out, cap, "Use a non-empty file name, without slashes, up to 63 bytes.");  // 拒绝本次服务修改并向调用者报告：Use a non-empty file name, without slashes, up to 63 bytes
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        int length = airi_snprintf(target, 128, "%s/%s", directory, name);  // 先拼当前目录与新名称；返回完整所需内容字节数，用于发现128字节数组的截断
        if (length >= 128)  // 所需内容已占满数组，NUL没有位置，拒绝截断路径以免误指另一文件
            return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, (size_t)length + 1))  // 只比较已占用槽的完整目标路径，长度加1连NUL一起匹配，重名不覆盖原记录
                return error(out, cap, "An item with that name already exists.");  // 拒绝本次服务修改并向调用者报告：An item with that name already exists
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (!entries[i].used) {  // 找到空服务槽后才能新建记录
                add(i, directory, name, act == UI_CREATE_DIR, act == UI_SAVE ? data : "");  // 新建槽按操作决定目录类型，新文档保存时填正文，其余创建为空
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Session storage capacity reached (32 items).");  // 拒绝本次服务修改并向调用者报告：Session storage capacity reached (32 items)
    }
    if (id < 1 || id > UI_FILES || !entries[id - 1].used)  // 拒绝1到32之外或已删除的服务id，检查完成前不访问id-1槽
        return error(out, cap, "The item no longer exists. Refresh the directory.");  // 拒绝本次服务修改并向调用者报告：The item no longer exists. Refresh the directory
    struct reference_file *f = &entries[id - 1];  // id已验证，减1找到对应服务槽
    if (act == UI_COPY_FILE || act == UI_MOVE_FILE) {  // 这两种操作使用name作为目标目录，并保留原普通文件名称与正文
        if (f->entry.directory)  // 目录没有普通文档内容，此操作需要拒绝
            return error(out, cap, "Directory copy requires a recursive filesystem provider.");  // 拒绝本次服务修改并向调用者报告：Directory copy requires a recursive filesystem provider
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        int length = airi_snprintf(target, 128, "%s/%s", name, f->entry.name);  // name在复制移动操作中是目标目录；保持原文件名，先检查组合路径能否完整保存
        if (length >= 128)  // 所需内容已占满数组，NUL没有位置，拒绝截断路径以免误指另一文件
            return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, (size_t)length + 1))  // 只比较已占用槽的完整目标路径，长度加1连NUL一起匹配，重名不覆盖原记录
                return error(out, cap, "The target already contains an item with that name.");  // 拒绝本次服务修改并向调用者报告：The target already contains an item with that name
        if (act == UI_MOVE_FILE) {  // 普通文件移动保留id，只修改完整路径
            airi_snprintf(f->entry.path, 128, "%s", target);  // 校验通过后提交新完整路径，文件服务id与正文保持不变
            return true;  // 该服务修改已完整提交，前端可重新查询当前记录
        }
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (!entries[i].used) {  // 找到空服务槽后才能新建记录
                add(i, name, f->entry.name, false, f->text);  // 复制普通文件到目标目录name，以同一名称和独立正文创建新id
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Session storage capacity reached.");  // 拒绝本次服务修改并向调用者报告：Session storage capacity reached
    }
    if (act == UI_SAVE) {  // 保存请求只修改这个服务记录的文字与长度
        if (f->entry.directory)  // 目录没有普通文档内容，此操作需要拒绝
            return error(out, cap, "Cannot write a directory as a document.");  // 拒绝本次服务修改并向调用者报告：Cannot write a directory as a document
        airi_snprintf(f->text, sizeof(f->text), "%s", data);  // 把此次保存正文复制到服务的8192字节数组，前端输入合同已将正文限制为8191字节
        f->entry.bytes = (unsigned)airi_strlen(f->text);  // 保存后以实际存储文字长度更新快照中的字节数
        return true;  // 该服务修改已完整提交，前端可重新查询当前记录
    }
    if (act == UI_DELETE) {  // 删除请求先检查目录是否仍含后代记录
        if (f->entry.directory) {  // 删除目录前检查全部服务槽，当前实现只允许删除没有后代的目录
            size_t len = airi_strlen(f->entry.path);  // 当前待删目录路径的字节长度，稍后用它定位后代路径中的斜杠边界
            for (unsigned i = 0; i < UI_FILES; ++i)   // 逐个检查服务32槽，查询或创建受当前占用状态约束
                if (entries[i].used && airi_strlen(entries[i].entry.path) > len &&  // 只检查占用槽且路径比待删目录长的项，目录自己不算自己的后代
                    !airi_memcmp(entries[i].entry.path, f->entry.path, len) &&  // 父路径字节必须完整相等，随后还需检查真正的斜杠层级边界
                    entries[i].entry.path[len] == '/')  // 相同前缀后还必须紧接斜杠，避免把projects2误当projects的后代
                    return error(out, cap, "The folder is not empty. Remove its contents first.");  // 拒绝本次服务修改并向调用者报告：The folder is not empty. Remove its contents first
        }
        f->used = false;  // 删除仅释放服务槽，之后查询不再返回此记录
        return true;      // 该服务修改已完整提交，前端可重新查询当前记录
    }
    if (act == UI_RENAME) {  // 重命名先验证目标名称与完整路径，再更新服务元信息
        if (airi_strlen(f->entry.name) == airi_strlen(name) &&  // 名称长度相同后才比较全部字节，同名重命名无需改服务记录
            !airi_memcmp(f->entry.name, name, airi_strlen(name) + 1))  // 连NUL比较当前名字，完全相同的重命名视为已完成
            return true;  // 该服务修改已完整提交，前端可重新查询当前记录
        if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
            return error(out, cap, "Invalid file name.");  // 拒绝本次服务修改并向调用者报告：Invalid file name
        if (f->entry.directory)  // 目录重命名还需同步改变全部后代路径，本参考服务不实现该操作
            return error(out, cap, "Folder renaming is unavailable.");  // 目录重命名需要同步后代路径，本参考服务明确返回未提供该能力。
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        airi_snprintf(target, sizeof(target), "%s", f->entry.path);  // 将原完整路径复制到128字节临时数组，随后只替换最后名称
        size_t parent = airi_strlen(target);  // 先从原路径文尾开始向前寻找最后斜杠，找到后保留其前面的父目录
        while (parent && target[parent - 1] != '/')  // 从原路径末尾向前找最后斜杠，保留父目录前缀
            --parent;  // 从路径尾部向前寻找最后一个目录分隔符
        int written = airi_snprintf(target + parent, sizeof(target) - parent, "%s", name);  // 在父目录后的剩余空间写新名称，返回未截断名称的所需字节数
        if (written < 0 || (size_t)written >= sizeof(target) - parent)  // 新名称未完整装入剩余空间时拒绝重命名，服务原路径仍保留
            return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
        size_t length = airi_strlen(target);     // 校验通过后的新完整路径长度，不含NUL，后续重名比较会加1
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, length + 1))  // 把新完整路径连NUL与已占用槽比较，完全同名时拒绝覆盖已有文件
                return error(out, cap, "An item with that name already exists.");  // 拒绝本次服务修改并向调用者报告：An item with that name already exists
        airi_snprintf(f->entry.path, 128, "%s", target);  // 校验通过后提交新完整路径，文件服务id与正文保持不变
        airi_snprintf(f->entry.name, 64, "%s", name);     // 复制单项名称到服务元信息，64字节数组最多容纳63内容字节与NUL
        return true;  // 该服务修改已完整提交，前端可重新查询当前记录
    }
    return error(out, cap, "Unsupported workspace operation.");  // 拒绝本次服务修改并向调用者报告：Unsupported workspace operation
}
static unsigned tasks(void *unused, struct ui_task *out, unsigned cap, struct ui_stats *stats) {  // 复制参考任务和同次参考资源统计到调用者快照
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    unsigned n = record_count < cap ? record_count : cap;  // 本次复制任务条数取有效记录与调用者数组容量的较小值
    airi_memcpy(out, records, n * sizeof(*out));  // 把本次可容纳的任务前缀复制给调用者快照
    *stats = (struct ui_stats){.available = true,  // 写调用者的同次资源统计快照，available表明参考服务确实提供了这些字段
                               .cpu = 37,  // 设置0到100的参考CPU百分数
                               .memory_kib = 104 * 1024,  // 设置参考已用内存量，单位KiB
                               .total_kib = 256 * 1024,   // 设置参考总内存量，单位KiB
                               .read_kib = 416,    // 设置参考每秒读量，单位KiB/s
                               .write_kib = 128};  // 设置参考每秒写量，单位KiB/s
    for (unsigned i = 0; i < 60; ++i) {  // 按采样下标处理60个参考百分数点
        unsigned triangle = i % 20;  // 把60个采样下标映射到每20点重复的三角周期
        stats->samples[i] = 19 + (triangle > 10 ? 20 - triangle : triangle) * 3 + (i * 17 % 13);  // 合成明确的参考百分数波形，数据不是CPU实测
    }
    return n;  // 返回本次已经复制到调用者数组的有效记录条数
}
void ui_fixture_backend(struct ui_backend *b) {  // 重置当前主机参考服务并填入回调表
    airi_memset(entries, 0, sizeof(entries));  // 把服务拥有的32个槽清空，再填本会话初始参考文件
    add(0, "/home/airi/projects/telemetry", "main.lua", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "-- Read a series and write a summary\nlocal values = {12, 18, 21, 32, 29, 37}\n\nlocal "  // main.lua正文的第一段；\n成为文档换行，无逗号的下一字符串继续同一正文
        "sum = 0\nfor _, value in ipairs(values) do\n    sum = sum + value\nend\n\nlocal mean = "  // main.lua正文继续保存求和循环；C相邻字符串拼接，这些Lua文字不在本次编译执行
        "sum / #values\nprint(string.format(\"mean: %.2f\", mean))\n");  // 均值示例只包含实际程序，不嵌入产品开发说明
    add(1, "/home/airi/projects/telemetry", "series.csv", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "seconds,load\n0,12\n1,18\n2,21\n3,32\n4,29\n5,37\n");  // 初始CSV含一行标题与6组时间秒数、测量整数；当前先作为普通文字存服务
    add(2, "/home/airi/projects/telemetry", "README.md", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "# Telemetry\n\nmain.lua computes the mean of six load samples.\n"  // 项目说明先介绍计算目的
        "series.csv stores the same observations in seconds and percent.\n"  // CSV列给出时间与负载单位
        "Open series.csv to inspect the time series.\n");  // 项目说明围绕输入、程序和数据查看操作
    add(3, "/home/airi/projects/telemetry", "output", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(6, "/home/airi/projects/telemetry", "sum.c", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "#include <stdio.h>\n\nint main(void) {\n    const int values[] = {12, 18, 21, 32};\n    "  // sum.c正文含示例包含行与main开头；\n形成文件换行，当前只存文字数据
        "int total = 0;\n    for (unsigned i = 0; i < 4; ++i)\n        total += values[i];\n    "  // sum.c正文继续保存求和循环；与前后字符串在C编译时拼成一个文档参数
        "printf(\"sum: %d\\n\", total);\n    return 0;\n}\n");  // 存入示例C文件最后打印与返回正文，这段字符串是数据而不是本次参与编译的C程序
    add(7, "/home/airi/projects/telemetry", "project.airi", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "[project]\nname = telemetry\nentry = main.lua\n");  // 存入项目说明文字，包含名字与入口；当前尚未把它当配置装载
    add(4, "/home/airi/projects", "telemetry", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(5, "/home/airi", "projects", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    const struct ui_task initial[] = {{1, "airidesk", "Running", 4, 12 * 1024},  // 第1条参考任务给pid、名称、状态、CPU百分数和12MiB的KiB量
                                      {2, "terminal", "Waiting", 0, 2 * 1024},  // 终端参考记录pid2处于等待，内存示例为2MiB
                                      {14, "series-worker", "Running", 31, 28 * 1024},  // 曲线worker参考记录pid14，CPU31%、内存28MiB
                                      {16, "editor", "Waiting", 2, 9 * 1024},  // Editor参考记录pid16，CPU2%、内存9MiB
                                      {18, "file-service", "Waiting", 0, 6 * 1024}};  // 文件服务参考记录pid18，内存6MiB，五项初始化结束
    airi_memcpy(records, initial, sizeof(initial));     // 把5条初始任务复制到服务拥有的固定数组
    record_count = sizeof(initial) / sizeof(*initial);  // 整组数组字节数除以每项大小得到初始5条任务
    boot = (AiriBootInfo){.magic = AIRI_BOOT_MAGIC,     // 为主机窗口准备固定参考启动结构，System明确标成示例数据
                          .version = AIRI_BOOT_VERSION,  // 设置启动合同版本
                          .size = sizeof(boot),  // 设置本版启动结构字节数
                          .direct_map_offset = UINT64_C(0xffff800000000000),  // 设置参考直接映射偏移
                          .kernel_physical_base = 0x100000,  // 设置参考内核物理起点
                          .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 设置参考内核虚拟起点
                          .kernel_size = 132 * 1024,  // 设置参考内核镜像长度
                          .region_count = 4};  // 设置参考内存区域有效条数
    airi_snprintf(boot.loader, 32, "UEFI inventory");  // 写明这份启动结构属于主机参考数据，System据此区别实际guest启动
    boot.regions[0] =  // 参考地图第0项描述最低1MiB保留范围，下一行填完整记录
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段名给起点0、长度1MiB和保留类别
    boot.regions[1] = (struct airi_memory_region){  // 参考地图第1项填写内核占用范围
        .base = 0x100000, .length = 0x100000, .kind = AIRI_MEMORY_KERNEL};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    boot.regions[2] = (struct airi_memory_region){  // 参考地图第2项填写当前保留的引导可回收范围
        .base = 0x200000, .length = 0x2900000, .kind = AIRI_MEMORY_BOOT_RECLAIM};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    boot.regions[3] = (struct airi_memory_region){  // 参考地图第3项填写可用物理范围
        .base = 0x2b00000, .length = 0xd500000, .kind = AIRI_MEMORY_USABLE};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    *b = (struct ui_backend){.mode = UI_REFERENCE,  // 通过b写调用者的整份回调表，函数地址交给files/read/action，未指定context字段自动为NULL
                             .boot = &boot,   // 设置借用启动信息，描述与内存地图须持续有效
                             .files = files,  // 保存目录查询函数地址，前端提供目录与数组，回调复制元信息而不出借服务槽
                             .read = read,    // 服务合同保存完整正文读取函数地址，桌面传服务id与调用者数组
                             .action = action,  // 保存参考文件/任务操作函数地址，前端请求经它修改服务表或取得失败说明
                             .tasks = tasks};   // 把任务和同次资源统计查询函数地址交给桌面，服务记录仍归参考后端
}
