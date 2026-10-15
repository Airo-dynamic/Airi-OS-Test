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
