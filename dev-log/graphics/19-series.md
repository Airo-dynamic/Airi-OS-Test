# G19 · 从 CSV 字节得到可绘制的曲线

Files能按服务id读取series.csv的文字，Viewer需要整数时间与测量值。现在沿参考文件中的seconds,load标题、0,12和1,18两行，把十进制字符转换成(0,12)与(1,18)，整份校验后发布成对数组，再按实际时间差画曲线。错误CSV保留旧数组。

曲线要同时保存时间与测量值，series_times[i]、series_values[i] 共同表示第 i 点，series_count 限定有效前缀，series_name 记录成功来源。在 ui.h 依次替换以下旧窗口，加入四项曲线状态并更新文档打开接口的说明；已有文件、文档与窗口状态继续保留。

<a id="code-frontend-include-ui-h-1"></a>

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files/Tasks列表行高，0为36像素、1为28像素；相应绘图和列表命中共用。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
```
<!-- after -->
```c
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files/Tasks列表行高，0为36像素、1为28像素；相应绘图和列表命中共用。
    unsigned series_count;  // 已成功解析的曲线点数，失败保留旧值。
    unsigned series_values[60];  // 各点纵轴整数值，与times同下标配对。
    unsigned series_times[60];   // 各点横轴秒数，按当前CSV合同递增。
    char series_name[64];  // 最近成功解析的数据文件名。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，按服务id读取普通文档并打开Editor。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
void ui_apps_click(struct ui_desktop *, enum ui_app, struct ui_rect, int, int, bool);  // 将内容矩形、屏幕点与双击状态交给具体应用。
```
<!-- after -->
```c
void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，读取id对应内容并选择Editor或Viewer。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
void ui_apps_click(struct ui_desktop *, enum ui_app, struct ui_rect, int, int, bool);  // 将内容矩形、屏幕点与双击状态交给具体应用。
```

<a id="csv-conversion"></a>

CSV 按行组织记录、以逗号分隔字段。这里把第一行跳过作为标题，不检查seconds/load列名；后面最多六十行非负整数，时间严格递增。每次乘十前要求累计值不超过100000，下一位最大为9，所以完整字段最多1000009；这是当前小型曲线演示的数值范围，超限会拒绝整份输入。当前不处理引号字段、小数、负号或逗号旁空格，这些格式会明确失败，而不会猜测转换。先用局部 times/values 保存结果，全部通过后才复制到桌面；这使第二行失败时第一行也不会成为半份新曲线。

在 apps/internal.h 末尾，以包含 ui_settings_click 和 endif 的旧窗口定位，加入曲线加载与绘制声明。旧块只用于定位，写回完整新块。

<a id="code-frontend-apps-internal-h-1"></a>

<!-- replace: frontend/apps/internal.h -->
<!-- before -->
```c
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
#endif  // 结束当前应用接口的重复包含保护
```
<!-- after -->
```c
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
void ui_viewer_draw(struct ui_desktop *, struct ui_rect);  // 把已提交时间和测量值缩放到真实时间比例的像素曲线
void ui_open_series(struct ui_desktop *, const struct ui_file *);  // 读取CSV并在整份验证通过后提交成对时间数值数组
#endif  // 结束当前应用接口的重复包含保护
```

新建 series.c。先按 f->id 把完整文字读进局部 csv，再越过首行；旧曲线此时没有改变，times、values 也只是本次准备提交的局部数组。

<a id="code-frontend-apps-series-c-1"></a>

<!-- write: frontend/apps/series.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数
void ui_open_series(struct ui_desktop *s, const struct ui_file *f) {  // 读取CSV并在整份验证通过后提交成对时间数值数组
    char csv[UI_DOC_SIZE];  // 8192字节局部数组完整承接这份CSV文字，解析只移动指针不改服务正文
    if (!s->backend.read || !s->backend.read(s->backend.context, f->id, csv, sizeof(csv))) {  // 缺读取回调或不能完整读取该服务id时停止解析，旧曲线数组仍保留
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unable to read data",  // 保存文字"Unable to read data"的种类、目标和说明，最后绘图且优先接收后续事件
                      "No data was returned by the file service.", "");  // 说明完整CSV读取失败；错误框没有名称初值，旧曲线仍保留
        return;  // CSV尚未完整读入，结束这次请求，旧点数和数组均不改变
    }
    unsigned times[60], values[60], count = 0;  // 局部数组暂存整份解析结果，count只计完整合法行
    bool valid = true;    // 尚未遇到非法记录，错误分支会将此标记清零
    const char *p = csv;  // 从服务返回的NUL文字首字节开始解析
    while (*p && *p != '\n')  // 越过首行标题，当前不校验标题拼写
        ++p;  // 消费当前文字字节后指针向后推进一字节
    if (*p)  // 当前尚未到NUL时才消费标题或记录行尾字节
        ++p;  // 消费当前文字字节后指针向后推进一字节
```

沿参考正文 `seconds,load\n0,12\n1,18\n`，先跳过首行。解析第一行时，时间字符0得到整数0，值字段的1令value从0变1，随后2使它变为1×10+2=12；遇换行后暂存(times[0],values[0])=(0,12)。第二行同样得到(1,18)，并确认时间1严格大于前点0，才计入第二个完整点。后续四行通过同样路径得到六点。解析失败保留旧曲线。

现在 p 位于第一条记录开头，继续追加同一个函数的逐行解析。每行先读时间和逗号，再读值；末尾可跳过一个回车，但随后仍必须是换行或NUL。count 只在一行全部通过后增加。

<!-- append: frontend/apps/series.c -->
```c
    while (*p) {  // 从当前记录首字节循环解析完整time,value行，遇正文NUL结束
        if (count == 60) {  // 最多接收60个点，额外数据行使整次解析失败
            valid = false;  // 标记整份新数据不可提交，旧曲线数组仍保留
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
        }
        unsigned time = 0, value = 0;  // 本行时间与测量值从0开始逐位累加
        bool digits = false;  // 记录当前字段是否至少读到一个数字
        while (*p >= '0' && *p <= '9') {  // 仅接受十进制数字字符，符号和小数会停在校验位置
            digits = true;  // 本字段已经有数字，空字段不再成立
            if (time > 100000) {  // 下一次乘10前检查时间累积值，限制数值增长
                valid = false;  // 标记整份新数据不可提交，旧曲线数组仍保留
                break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
            }
            time = time * 10 + (unsigned)(*p - '0');  // 原值左移一个十进制位，字符减0的编码取得当前数字
            ++p;  // 消费当前文字字节后指针向后推进一字节
        }
        if (!valid || !digits || *p != ',') {  // 前字段必须合法且非空，后面必须紧接逗号
            valid = false;  // 标记整份新数据不可提交，旧曲线数组仍保留
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
        }
        ++p;  // 消费当前文字字节后指针向后推进一字节
        digits = false;  // 开始测量值字段，重新要求至少一个数字
        while (*p >= '0' && *p <= '9') {  // 仅接受十进制数字字符，符号和小数会停在校验位置
            digits = true;  // 本字段已经有数字，空字段不再成立
            if (value > 100000) {  // 下一次乘10前检查测量值累积数值
                valid = false;  // 标记整份新数据不可提交，旧曲线数组仍保留
                break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
            }
            value = value * 10 + (unsigned)(*p - '0');  // 逐位把测量值的十进制字符转换成整数
            ++p;  // 消费当前文字字节后指针向后推进一字节
        }
        if (*p == '\r')  // 允许跳过数值后的一个回车字节，随后仍须校验换行或文尾NUL
            ++p;  // 消费当前文字字节后指针向后推进一字节
        if (!valid || !digits || (*p && *p != '\n') || (count && time <= times[count - 1])) {  // 要求值非空、行尾正确且时间严格大于上一个完整点
            valid = false;  // 标记整份新数据不可提交，旧曲线数组仍保留
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
        }
        times[count] = time;      // 把本行时间放到即将提交的同一下标
        values[count++] = value;  // 先在旧下标保存测量值，再把完整点数加一
        if (*p)  // 当前尚未到NUL时才消费标题或记录行尾字节
            ++p;  // 消费当前文字字节后指针向后推进一字节
    }
```

循环结束后，count 与两块局部数组才代表这一整份候选结果。只要 valid=false 或一条点都没有，就只复制错误说明，桌面旧数组和来源名都保留。全部通过才写 series_count、复制两份同长度前缀、记来源名并打开 Viewer；这些操作在同一次同步调用中完成，绘图在返回后读取。继续追加校验出口与提交。

<!-- append: frontend/apps/series.c -->
```c
    if (!valid || !count) {  // 任何非法记录或无有效点都保留旧曲线并报告错误
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Invalid series",  // 保存文字"Invalid series"的种类、目标和说明，最后绘图且优先接收后续事件
                      "Use a header and up to 60 integer time,value rows; timestamps must "  // 非法CSV错误说明的前半；无逗号的相邻下一字符串拼接成同一个body参数
                      "increase. The previous plot is preserved.",  // 与前一段拼成完整失败说明；这一逗号结束body参数，下行空串提供输入初值
                      "");  // 此独立实参是空输入值，错误框不需要编辑字段
        return;  // 没有完整合法新数据，错误框已设置，旧点数和数组保持原值
    }
    s->series_count = count;  // 全部校验后开始提交有效点数；同次函数随后复制成对数组，返回后绘图才读取新状态
    airi_memcpy(s->series_times, times, count * sizeof(*times));     // 只复制有效时间前缀，sizeof每项决定字节数
    airi_memcpy(s->series_values, values, count * sizeof(*values));  // 按同样有效点数复制测量值，保持两个数组下标配对
    airi_snprintf(s->series_name, 64, "%s", f->name);  // 给成功提交的曲线保存来源文件名称
    ui_open(s, UI_VIEWER);  // 已验证曲线提交后打开并置顶Viewer，绘图从当前desktop数组读取
    return;  // 新曲线及来源名已提交，Viewer已打开，结束本次加载
}
```

解析器已有成功发布与失败保留两条出口。现在把 Files 打开路径接入它：本参考快照第1项为 series.csv、服务 id=2，ui_open_document 先处理目录，再检查名称尾部是否恰为小写 .csv。CSV 直接交 ui_open_series，不覆盖 Editor；所以即使 Editor 仍有未保存文字，也可另外打开数据。其余普通文档继续走原来的 dirty 保护与完整读取。

在 documents.c 依次替换以下旧窗口，加入 CSV 分流并保留各条返回路径。按列出的旧文本定位，不重写其余 Editor 状态提交。

<a id="code-frontend-apps-documents-c-1"></a>

<!-- replace: frontend/apps/documents.c -->
<!-- before -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_open_document(struct ui_desktop *s, unsigned index) {  // 按快照下标打开目录或读取文档，成功才切换编辑身份
    if (index >= s->file_count)  // 快照下标必须落在这次查询的有效前缀
        return;  // 快照下标越界，不读取条目或改变当前文档
    struct ui_file *f = &s->files[index];  // 借用快照第index项，后面用其服务id而非下标读取
    if (f->directory) {  // 目录项改变当前目录，不当成文字文档读取
        airi_snprintf(s->directory, 128, "%s", f->path);  // 用目录记录的完整路径替换当前查询目录
        ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
        return;  // 已切换查询目录并刷新快照，Editor 文档保持原样
    }
    if (s->dirty) {  // 当前文档未保存时先阻止另一个文档覆盖编辑数组
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unsaved changes",  // 保存文字"Unsaved changes"的种类、目标和说明，最后绘图且优先接收后续事件
                      "Save or discard the current document before opening another file.", "");  // 说明未保存正文阻止新文档覆盖，最后空串表示此错误框没有名称初值
        return;  // 当前文档未保存，显示提示并保留工作数组
    }
    if (!s->backend.read ||  // 先检查读取函数地址是否存在，短路或保证缺回调时不调用空函数指针
```
<!-- after -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_open_document(struct ui_desktop *s, unsigned index) {  // 按快照下标选择目录、CSV或文档，成功后切换对应查询或视图
    if (index >= s->file_count)  // 快照下标必须落在这次查询的有效前缀
        return;  // index不在有效快照内，不读取服务记录也不改变当前视图
    struct ui_file *f = &s->files[index];  // 借用快照第index项，后面用其服务id而非下标读取
    if (f->directory) {  // 目录项改变当前目录，不当成文字文档读取
        airi_snprintf(s->directory, 128, "%s", f->path);  // 用目录记录的完整路径替换当前查询目录
        ui_refresh(s);  // 按新的directory重新查询文件，同时刷新已有任务与资源快照
        return;  // 目录查询已刷新，结束目录路径，不继续把它当CSV或文档读取
    }
    size_t name_len = airi_strlen(f->name);  // 测量名称长度，以便在不越界的前提下检查后缀
    if (name_len >= 4 && !airi_memcmp(f->name + name_len - 4, ".csv", 4)) {  // 先保证至少4字节，再比较尾部.csv决定进入曲线解析
        ui_open_series(s, f);  // 使用该文件的服务id读取并解析曲线，成功才发布数组
        return;  // CSV读取与校验已完成或报告失败，结束曲线路径，不覆盖Editor正文
    }
    if (s->dirty) {  // 当前文档未保存时先阻止另一个文档覆盖编辑数组
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unsaved changes",  // 保存文字"Unsaved changes"的种类、目标和说明，最后绘图且优先接收后续事件
                      "Save or discard the current document before opening another file.", "");  // 说明未保存正文阻止新文档覆盖，最后空串表示此错误框没有名称初值
        return;  // 当前Editor有未保存修改，保留其文字和身份并结束本次打开
    }
    if (!s->backend.read ||  // 先检查读取函数地址是否存在，短路或保证缺回调时不调用空函数指针
```

<!-- replace: frontend/apps/documents.c -->
<!-- before -->
```c
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unable to open file",  // 保存文字"Unable to open file"的种类、目标和说明，最后绘图且优先接收后续事件
                      "The file service did not return the document.", "");  // 说明读取服务失败，错误框无需名称字段，原编辑正文保持完整
        return;  // 读取回调缺失或读取失败，保留原文与编辑身份
    }
    s->editor_id = f->id;  // 读取成功后才让编辑器绑定新文件的服务身份
```
<!-- after -->
```c
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unable to open file",  // 保存文字"Unable to open file"的种类、目标和说明，最后绘图且优先接收后续事件
                      "The file service did not return the document.", "");  // 说明读取服务失败，错误框无需名称字段，原编辑正文保持完整
        return;  // 完整读取失败，错误框已设置，保留当前文档身份和保存点
    }
    s->editor_id = f->id;  // 读取成功后才让编辑器绑定新文件的服务身份
```

<a id="series-scaling"></a>

解析出的 (time,value) 才是曲线输入。若时间为 0、10、30，第三点离起点的距离应是第二点的三倍；直接按数组下标均分会丢掉这种意义。因此横坐标用真实时间差，纵坐标用当前最大测量值缩放。这里的整数除法丢弃小数部分，得到离散像素位置。

Viewer读取ui_open_series整份验证后提交到当前desktop的series_times和series_values。横坐标使用相对首点的时间差除以 duration，因此不同时间间隔形成不同距离；纵坐标用 value/max 缩放。max 初值为 1，单点 duration 也取 1，避免零除。只有一个点时不会进入连接相邻点的循环，不连线，仍显示时间标签、刻度和该观测点的空心方标记。

中间乘法先转 uint64_t。没有已加载点或服务状态非正常时，显示相应提示，图中只出现成功解析的数据。

**新建 `frontend/apps/viewer.c`：**

<a id="code-frontend-apps-viewer-c-1"></a>

<!-- write: frontend/apps/viewer.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_viewer_draw(struct ui_desktop *s, struct ui_rect r) {  // 把已提交时间和测量值缩放到真实时间比例的像素曲线
    ui_app_toolbar(r, s->series_name[0] ? s->series_name : "Data viewer");  // 在内容顶端铺44像素工具栏并显示按当前状态选择的文案
    u_button((struct ui_rect){r.x + r.w - 108, r.y + 8, 92, 28}, "Open files", false, false);  // 画文字"Open files"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_label(r.x + 24, r.y + 67, "TIME SERIES", U_MUTED);  // 在内容左缘右移24像素、顶边下移67像素显示时间序列分区身份。
    ui_app_muted(r.x + 24, r.y + 102, "Time / seconds");  // 在内容左缘右移24像素、内容顶边下移102像素显示文字"Time / seconds"
    if (s->service_state != UI_NORMAL || !s->series_count) {  // 服务不正常或没有成功提交曲线时仅显示提示，不绘造假的数据线
        ui_app_unavailable(s, (struct ui_rect){r.x, r.y + 44, r.w, r.h - 72},  // 按服务状态绘制加载、空或不可用提示，本次实参取自当前桌面状态
                           "Open a two-column CSV document from Files.");  // 提示从Files打开两列CSV的实际入口，尚未加载时不显示参考假曲线
        ui_app_status(r, "No series loaded", NULL);  // 在内容底部显示文字"No series loaded"；右侧说明按测得的文字宽度对齐
        return;  // 服务状态不可用或尚无已提交曲线，只显示提示，不缩放或连点
    }
    struct ui_rect graph = {r.x + 52, r.y + 156, r.w - 84, r.h - 241};  // 曲线图区扣左侧52像素标签、顶156像素工具栏及底部空间，尺寸仍按像素
    unsigned max = 1;  // 纵轴上限至少为1，使全零序列也不除零
    for (unsigned i = 0; i < s->series_count; ++i)  // 只遍历已经整份验证并提交的曲线点
        if (s->series_values[i] > max)  // 当前测量值超过现有纵轴上限时更新上限，较小值不缩短比例范围
            max = s->series_values[i];  // 遇到更大测量值就更新纵轴缩放上限
    for (unsigned i = 0; i < 4; ++i) {  // 给测量值纵轴画四个整数刻度和网格线
        int y = graph.y + (int)i * graph.h / 3;  // 第i条网格沿曲线高度的三等分排列，i从0到3包含上下边界
        u_rect(graph.x, y, graph.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(graph.x,y)，尺寸graph.w×1像素
        char n[24];  // 24字节局部数组保存这条网格对应的整数测量值刻度
        airi_snprintf(n, 24, "%u", max - i * max / 3);  // 当前纵轴网格从最大测量值降到0，用实际数据范围形成刻度
        ui_app_muted(r.x + 20, y - 7, n);  // 在内容左缘右移20像素、整屏y=y - 7显示当前纵轴数值文字
    }
```

纵轴网格已按当前最大值形成，下面才用首尾时间得到 duration，并将每个有效点换成图内坐标。相邻点连线后再画空心方标记：线说明点之间的连接，方框标明真正存在的观测。首末时间标签与状态栏继续读取同一有效前缀。继续追加 Viewer 的坐标换算与收尾。

<!-- append: frontend/apps/viewer.c -->
```c
    unsigned duration = s->series_times[s->series_count - 1] - s->series_times[0];  // 最后时间减首时间得到整幅横轴表示的实际跨度
    if (!duration)  // 单个数据点的时间跨度为0时采用非零分母
        duration = 1;  // 仅为缩放计算保留1，原始时间数组不修改
    for (unsigned i = 1; i < s->series_count; ++i)  // 只遍历已经整份验证并提交的曲线点
        u_line(graph.x + (int)((uint64_t)(s->series_times[i - 1] - s->series_times[0]) *  // 上一点时间减首时间后乘图宽，随后除总跨度得到真实时间比例的起点x
                               (unsigned)graph.w / duration),  // 把已知为正的图宽按像素参与64位乘法，再除时间跨度得到前一点横向像素距离
               graph.y + graph.h -  // 纵轴从图底向上扣除测量值对应的像素高度
                   (int)((uint64_t)s->series_values[i - 1] * (unsigned)graph.h / max),  // 前一点测量值乘图高再除纵轴上限，64位计算后转成int像素高度
               graph.x + (int)((uint64_t)(s->series_times[i] - s->series_times[0]) *  // 当前点真实时间减首时间，先升64位再乘图宽避免中间溢出
                               (unsigned)graph.w / duration),  // 把已知为正的图宽按像素参与64位乘法，再除时间跨度得到当前点横向像素距离
               graph.y + graph.h - (int)((uint64_t)s->series_values[i] * (unsigned)graph.h / max),  // 当前值按纵轴上限换算为高度，再从图底扣除得到屏幕y
               U_ACCENT);  // 当前数据图元使用会话强调色
    for (unsigned i = 0; i < s->series_count; ++i) {  // 观测点与插值连线使用不同形状
        int px = graph.x + (int)((uint64_t)(s->series_times[i] - s->series_times[0]) * (unsigned)graph.w / duration);  // 横坐标取实际时间
        int py = graph.y + graph.h - (int)((uint64_t)s->series_values[i] * (unsigned)graph.h / max);  // 纵坐标取实际值
        u_rect(px - 3, py - 3, 7, 7, U_SURFACE);  // 隔开线段与采样标记
        u_frame((struct ui_rect){px - 2, py - 2, 5, 5}, U_ACCENT);  // 空心方点只表示存在的观测
    }
    char first[24], last[24];  // 两个24字节数组分别保存首时间和末时间标签，绘图期间同时保留
    airi_snprintf(first, 24, "%u s", s->series_times[0]);  // 形成CSV首点时间的秒数标签，显示在横轴左端
    airi_snprintf(last, 24, "%u s", s->series_times[s->series_count - 1]);  // 形成CSV末点时间的秒数标签，显示在横轴右端
    ui_app_muted(graph.x, graph.y + graph.h + 13, first);  // 在整屏x=graph.x、整屏y=graph.y + graph.h + 13显示首个时间标签
    ui_app_muted(graph.x + graph.w - u_text_width(last, U_SMALL), graph.y + graph.h + 13, last);  // 在整屏x=graph.x + graph.w - u_text_width(last, U_SMALL)、整屏y=graph.y + graph.h + 13显示最后时间标签
    char description[80];  // 80字节状态文字保存实际已提交点数与来源CSV文件名称
    airi_snprintf(description, 80, "%u rows / %s", s->series_count, s->series_name);  // 状态栏报告实际提交点数与来源文件名，验证失败时仍显示旧曲线身份
    ui_app_status(r, description, "CSV data");  // 在内容底部显示曲线条数和来源名称；右侧说明按测得的文字宽度对齐
}

```

在 builtin.c 的 Settings 与 default 之间接入 Viewer 绘制，替换下面唯一旧窗口。点击分派继续保留本章已有内容。

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
        ui_settings_draw(s, r);  // 用当前桌面和内容矩形绘制设置。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
        break;  // 当前匹配已完成，停止这一层循环或分支。
```
<!-- after -->
```c
        ui_settings_draw(s, r);  // 用当前桌面和内容矩形绘制设置。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_VIEWER:  // 应用身份为曲线时进入对应分支。
        ui_viewer_draw(s, r);  // 用当前桌面和内容矩形绘制曲线。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
        break;  // 当前匹配已完成，停止这一层循环或分支。
```

## 在图片进程中读取同一份CSV

数值观察和图片观察是两次独立启动的主机程序，各自初始化reference、console和desktop。build/series退出后，它的数组不会传给build/desktop。图片程序因此在应用参数为Viewer时，先按本参考快照第1项读取CSV，再绘制当前进程的已提交数组；其他应用的观察路径保持原样。

图片程序仍使用同一 main。依次替换下面的唯一旧窗口，更新初始化说明、接入 Viewer 参数下的 CSV 加载，并更新绘图调用说明。真正新增的读取发生在打开并最大化所选应用之前；其他输出步骤保留。

<a id="code-labs-tutorial-desktop-c-1"></a>

<!-- replace: labs/tutorial/desktop.c -->
<!-- before -->
```c
int main(int argc, char **argv) {    // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 重置当前主机参考服务并填入回调表，本次实参取自当前桌面状态
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 清桌面状态，绑定服务和console并打开默认应用，本次实参取自当前桌面状态
    int app = argc > 1 ? atoi(argv[1]) : UI_TERMINAL;  // 有参数时把十进制字符串变成应用编号，否则生成Terminal
    if (app < 0 || app >= UI_APP_COUNT)  // 检查转换结果的数组边界后才索引窗口
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    ui_open(&desktop, (enum ui_app)app);    // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
    desktop.windows[app].maximized = true;  // 观察程序先把目标窗口设为最大化，绘图据此计算实际矩形
```
<!-- after -->
```c
int main(int argc, char **argv) {    // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 把服务回调写入本观察程序的 backend 结构，并重置参考静态记录
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 同一desktop借用console并复制后端合同，后续每次操作都修改这份状态
    int app = argc > 1 ? atoi(argv[1]) : UI_TERMINAL;  // 有参数时把十进制字符串变成应用编号，否则生成Terminal
    if (app < 0 || app >= UI_APP_COUNT)  // 检查转换结果的数组边界后才索引窗口
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    if (app == UI_VIEWER)  // 图片进程需要自己加载数据，不能借用另一个进程的数组
        ui_open_document(&desktop, 1);  // 本参考快照第1项是series.csv，成功解析后发布六个点
    ui_open(&desktop, (enum ui_app)app);    // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
    desktop.windows[app].maximized = true;  // 观察程序先把目标窗口设为最大化，绘图据此计算实际矩形
```

<!-- replace: labs/tutorial/desktop.c -->
<!-- before -->
```c
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 设置每行实际字节跨度、每像素位数、红色通道位数、红色通道最低位位置
        .green_size = 8, .green_shift = 8, .blue_size = 8};  // 设置绿色通道位数、绿色通道最低位位置、蓝色通道位数
    ui_draw(&desktop, &fb);  // 按当前窗口次序绘制整帧，本次实参取自当前桌面状态
    FILE *file = fopen("build/desktop.ppm", "wb");  // 以二进制写并截断旧内容打开本次桌面PPM文件
    if (!file)  // 打开PPM输出失败时不继续写文件
```
<!-- after -->
```c
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 设置每行实际字节跨度、每像素位数、红色通道位数、红色通道最低位位置
        .green_size = 8, .green_shift = 8, .blue_size = 8};  // 设置绿色通道位数、绿色通道最低位位置、蓝色通道位数
    ui_draw(&desktop, &fb);  // 按窗口层次画整帧，并将对话框留在最后，本次实参取自当前桌面状态
    FILE *file = fopen("build/desktop.ppm", "wb");  // 以二进制写并截断旧内容打开本次桌面PPM文件
    if (!file)  // 打开PPM输出失败时不继续写文件
```

长应用名还需要正确呈现。G15的u_label_width按真实字形advance加每字符两像素测量身份，片宽在54像素左部空间之外再留12像素给斜切边缘。下面先画Viewer一帧，并沿相同推进量取得最后R的包围盒：每个覆盖率都应混入 U_CHROME，身份片须覆盖最后一个字母的有效像素。观察程序分别比较字形实心、部分覆盖和空白，再检查 CSV 失败时旧数组是否保留。

**完整写入 `labs/tutorial/series.c`：**

<a id="code-labs-tutorial-series-c-1"></a>

<!-- write: labs/tutorial/series.c -->
```c
#include "../../frontend/assets/font-atlas.h"  // 只读第12章生成的相同字形，核对实际标题末R的像素。
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 观察程序使用自有数组复制与比较检查曲线是否保留
#include <stdio.h>      // 主机观察使用标准文件输出与打印接口
static struct ui_desktop desktop;    // 主机观察拥有整份静态桌面，多个步骤修改同一对象
static struct airi_console console;  // 主机观察拥有同一console输入输出状态
static uint32_t title_pixels[1280 * 800];  // 观察进程拥有一帧像素，渲染器只借用这块数组。
static bool title_fits(void) {  // 校验DATA VIEWER最后R的字形与它下面的身份片。
    desktop.windows[UI_VIEWER].maximized = true;  // 先采用当前画布的真实最大化布局。
    struct airi_framebuffer canvas = {  // 与之前图片观察相同的紧排32位RGB布局。
        .virtual_address = (uintptr_t)title_pixels, .width = 1280, .height = 800,  // 地址属于本进程，宽高单位为像素。
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 每行5120字节，红色占16..23位。
        .green_size = 8, .green_shift = 8, .blue_size = 8  // 其余通道构成标准RGB数值，未指定最低位为零。
    };
    ui_draw(&desktop, &canvas);  // 先更新窗口实际rect，再读取这一帧的像素。
    struct ui_rect r = desktop.windows[UI_VIEWER].rect;  // 后面的标题坐标使用本次绘图回写的外框。
    const char *label = "DATA VIEWER";  // 观察当前应用完整身份，不替换成更短的测试文字。
    int x = r.x + 62;  // 第一字符原点，与窗口绘图采用的标签左缘相同。
    for (const char *p = label; p[1]; ++p) {  // 只累计末字母R之前的十个字符。
        char ch[2] = {*p, 0};  // 当前一字节加NUL，交给相同字体的宽度计算。
        x += u_text_width(ch, U_MEDIUM) + 2;  // 字形推进和额外字距都必须计入末R位置。
    }
    const struct u_glyph *g = &u_glyphs[U_MEDIUM]['R' - 32];    // 同一生成图集给出R的覆盖率、偏移和包围盒。
    uint32_t ink = u_color(U_INK), chrome = u_color(U_CHROME);  // 取得当前主题的标题前景与身份片背景。
    for (unsigned row = 0; row < g->h; ++row)  // 每个R字形行都参与比较，不只看一个碰巧实心的像素。
        for (unsigned col = 0; col < g->w; ++col) {  // 也检查字形空白与部分覆盖处的背景。
            unsigned alpha = u_coverage[g->offset + row * g->w + col];  // 从相同A8数组取得当前像素覆盖率。
            uint32_t expected = 0;  // 逐个八位通道形成应有的混合RGB。
            for (unsigned k = 0; k < 3; ++k) {  // 按红绿蓝顺序使用第12章同一混合公式。
                unsigned shift = (2 - k) * 8;  // 三个通道的最低位依次为16、8、0。
                unsigned before = (chrome >> shift) & 255u;  // R下方应是身份片，而不是框架底色。
                unsigned after = (ink >> shift) & 255u;      // 当前字形的前景通道。
                unsigned channel = (after * alpha + before * (255 - alpha) + 127) / 255;  // 部分覆盖像素也必须混入正确的片背景。
                expected = (expected << 8) | channel;  // 每轮追加一个八位通道，形成RGB。
            }
            int px = x + g->x + (int)col, py = r.y + 14 + g->y + (int)row;  // 字形偏移加局部行列得到实际画布位置。
            if (title_pixels[py * 1280 + px] != expected)  // 旧174像素身份片会让末R部分像素读到错误背景。
                return false;  // 标签虽写了字母，跨出身份片仍属于布局失败。
        }
    puts("title: DATA VIEWER final R stays on the identity plate");  // 报告完整字形与背景逐像素核对的结果。
    return true;  // 当前长标签已经由真实字体度量得到足够承载范围。
}

```

title_fits 已有了按真实标题字形逐像素核对的路径，main 接下来先在本进程初始化服务并打开快照第1项。确认六个点已提交且 active=UI_VIEWER，才调用标题观察或用 count-1 取末项；不能在加载失败时自行补一个假点。继续追加 main 的正常路径。

<!-- append: labs/tutorial/series.c -->
```c
int main(void) {  // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 把服务回调写入本观察程序的 backend 结构，并重置参考静态记录
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 同一desktop借用console并复制后端合同，后续每次操作都修改这份状态
    ui_open_document(&desktop, 1);  // 按快照下标打开目录或读取文档，成功才切换编辑身份，本次实参取自当前桌面状态
    if (desktop.series_count != 6 || desktop.active != UI_VIEWER)  // 先核对解析成功与点数，后面才能安全使用count-1下标
        return 1;  // 没有预期有效点时停止观察，不索引未提交数组
    if (!title_fits())  // Viewer已载入六点，当前标题也必须完整留在身份片上。
        return 1;  // 长身份文字跨片会在字形的背景像素比较中被发现。
    printf("points=%u first=%u,%u last=%u,%u active=%d\n", desktop.series_count,  // 向主机终端报告本次观察的实际状态或输出位置
           desktop.series_times[0], desktop.series_values[0],  // 报告当前进程已提交曲线首点的真实时间与值
           desktop.series_times[desktop.series_count - 1],     // 以有效点数减1定位最后时间，前提是解析已有点
           desktop.series_values[desktop.series_count - 1], desktop.active);  // 同一下标取得最后测量值，并报告当前活动应用编号
```

正常六点与标题观察完成后，继续在同一进程构造失败。先独立复制当前六个时间和六个值，再通过服务把 CSV 改成时间0重复的两行。服务只保存字节，所以保存会成功；再次打开时解析器才在第二条时间0处拒绝。此时要求错误框、series_count=6 和旧数组各字节都保持，不能只看点数。

<!-- append: labs/tutorial/series.c -->
```c
    unsigned old_times[60], old_values[60];  // 独立保存当前已验证数组，稍后检查非法输入没有改任何有效点
    airi_memcpy(old_times, desktop.series_times, 6 * sizeof(*old_times));  // 保存6个时间值，数组尾部不属于当前有效前缀
    airi_memcpy(old_values, desktop.series_values, 6 * sizeof(*old_values));  // 保存相同6个测量值，与时间下标继续配对
    char error[160];  // 160字节数组承接服务写入失败说明，此观察不会借用错误临时指针
    if (!backend.action(backend.context, desktop.directory, UI_SAVE, desktop.files[1].id,  // 以CSV项服务id提交测试正文，快照下标1只负责找到该id
                        "series.csv", "seconds,load\n0,12\n0,18\n", error, sizeof(error)))  // 第二行仍为时间0，故意违反严格递增；当前服务只保存文字不替解析器校验
        return 1;  // 若服务无法接受测试正文，则还没有构造好本次非法CSV观察
    ui_open_document(&desktop, 1);  // 再次打开同一CSV，解析器在第二个时间0处拒绝整份数据
    if (desktop.dialog != UI_DIALOG_ERROR || desktop.series_count != 6 ||  // 要求明确错误框且旧有效点数仍是6
        airi_memcmp(old_times, desktop.series_times, 6 * sizeof(*old_times)) ||  // 任何旧时间字节改变都算保留合同失败
        airi_memcmp(old_values, desktop.series_values, 6 * sizeof(*old_values)))  // 任何旧测量值改变同样失败，不能仅看点数
        return 1;  // 解析失败误发布部分新数据时报告观察失败
    puts("invalid timestamps -> previous 6 points preserved");  // 报告第二次读取被拒绝，同时旧曲线完整保留
    return 0;  // 正常六点与非法时间保留路径都通过观察
}
```

数值观察中的服务正文现在是非法CSV，但这份修改只活在当前进程。图片程序另行启动，会重新初始化正常参考CSV，再走自己的完整读取与解析，所以故障观察不会污染随后生成的图片。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/series.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/series  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/series  # 运行当前完整观察程序，退出非零表示本组观察失败
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/desktop.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/desktop  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/desktop  # 运行当前完整观察程序，退出非零表示本组观察失败
./build/desktop 6  # 另一个进程重新加载自己的series.csv，再绘制六点曲线
```

build/series打印六个点，首尾为(0,12)与(5,37)，active为Viewer的编号6；重复时间随后得到`invalid timestamps -> previous 6 points preserved`。随后build/desktop 6在新进程重新解析同一参考CSV，生成包含六点曲线的build/desktop.ppm。打开图片可见实际时间比例的折线。下一步让鼠标选择文件并操作 Editor。

标题观察先输出`title: DATA VIEWER final R stays on the identity plate`，随后继续打印六点和非法时间保留结果。
