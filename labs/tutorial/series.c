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
