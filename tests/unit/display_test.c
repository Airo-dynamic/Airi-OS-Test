#include <assert.h>  // 断言实际绘图结果。
#include <stdlib.h>  // 为带哨兵的宿主画布分配空间。
#include <string.h>  // 初始化哨兵字节。
#include <stdint.h>  // 画布地址使用固定宽整数。
#include <stdio.h>   // 输出本轮完成记录。
#include <airi/kernel.h>   // 借用启动描述与图形适配声明。
#include <airi/console.h>  // 通过同一文本命令模型切换视图。
static AiriBootInfo fixture(void) {  // 构造合法启动输入，各故障都从它的独立副本开始。
    AiriBootInfo b = {.magic = AIRI_BOOT_MAGIC,  // 指定magic识别协议，未指定成员按C规则清零。
                      .version = AIRI_BOOT_VERSION,  // 采用当前接口版本。
                      .size = sizeof(AiriBootInfo),  // 字节大小与编译的结构布局一致。
                      .direct_map_offset = UINT64_C(0xffff800000000000),  // UINT64_C明确64位常量，用作物理直接映射偏移。
                      .kernel_physical_base = 0x100000,  // 构造内核物理起点1MiB。
                      .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 内核虚拟起点与链接模型对应。
                      .kernel_size = 4096,  // 构造一页4096字节内核长度。
                      .region_count = 2};   // 只有两条内存区域有效。
    b.regions[0] =  // 第一项描述0..1MiB保留区域。
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段赋值，物理范围不标为可分配。
    b.regions[1] = (struct airi_memory_region){  // 第二项从1MiB开始，紧邻首项且不重叠。
        .base = 0x100000, .length = 0x400000, .kind = AIRI_MEMORY_USABLE};  // 1..5MiB共4MiB构造可用范围。
    return b;  // 按值返回整个结构，调用者得到可独立损坏的副本。
}
static void type(struct airi_console *c, const char *s) {  // 把完整命令按字节送入同一个console，再回车执行。
    while (*s)  // 当前字节不是NUL才继续输入。
        airi_console_key(c, *s++);  // 先取当前字节送键事件，再将源指针推进。
    airi_console_key(c, '\n');  // 命令末尾明确发送提交键，不把NUL当按键。
}
static void graphics_tests(void) {  // 让真实共享绘图在主机有界画布运行，核对两端和行填充。
    /* Guarded 24/32-bit framebuffer with padded scanlines: exercise real renderer. */
    const unsigned layouts[] = {24, 32, 32};  // 前两次640×480分别24/32位，第三次1280×800采用32位。
    for (size_t k = 0; k < 3; ++k) {  // 遍历上述三组实际布局。
        AiriBootInfo b = fixture();  // 在当前调用栈中取得合法完整副本。
        unsigned width = k == 2 ? 1280 : 640, height = k == 2 ? 800 : 480;  // 第三组使用大画布，其余使用最小支持尺寸。
        uint64_t pitch = width * (layouts[k] / 8) + 16, size = pitch * height;  // 像素位数换字节，每行附16字节填充，总字节数包含它。
        unsigned char *pixels = malloc((size_t)size + 64);  // 画布两侧各多分配32字节哨兵。
        assert(pixels);  // 分配有效后才可写填充和调用绘图。
        memset(pixels, 0xa5, (size_t)size + 64);  // 可见区域、行填充和首尾都先置同一哨兵。
        b.framebuffer = (struct airi_framebuffer){.virtual_address = (uintptr_t)(pixels + 32),  // 画布地址跳过前端哨兵，后端仍预留32字节。
                                                  .width = width,    // 按本轮选择的像素宽度绘图。
                                                  .height = height,  // 使用同组高度。
                                                  .pitch = pitch,    // 行地址必须按包含填充的字节跨度计算。
                                                  .bpp = (uint16_t)layouts[k],  // 结构要求16位字段，当前24/32均在其范围内。
                                                  .red_size = 8,     // 红通道占八位。
                                                  .red_shift = 16,   // 红通道放在位16..23，读写需平移16位。
                                                  .green_size = 8,   // 绿通道占八位。
                                                  .green_shift = 8,  // 绿通道放在位8..15。
                                                  .blue_size = 8};   // 蓝通道占八位，未指定 blue_shift 时默认零。
        assert(airi_framebuffer_valid(&b.framebuffer));  // 当前布局应满足地址、范围与颜色位合同。
        assert(airi_dashboard_draw(&b));  // 仪表台必须实际在该布局完成一帧。
        struct airi_console c;      // 独立命令状态在当前检查期间有效。
        airi_console_init(&c, &b);  // 借用当前合法启动描述，初始化命令与日志。
        assert(airi_console_draw(&c));  // 从仪表台切换到另一份输入会话，先为它绘制首帧。
        assert(c.extension);  // 新会话必须已安装自己的图形命令回调，不能沿用仪表台的绑定。
        airi_console_init(&c, &b);  // 同一地址也可成为重置后的新会话，初始化会清空扩展。
        assert(!c.extension);  // 确认这次重置确实撤下了此前的命令消费者。
        assert(airi_console_draw(&c));  // 地址未变也必须发现绑定失效并重建桌面。
        assert(c.extension);  // 重置后的会话重新获得自己的主题命令入口。
        unsigned char *sample = pixels + 32 + (height - 1) * pitch + (width - 1) * (layouts[k] / 8);  // 取底栏最右像素的前三字节，避开文字和窗口。
        unsigned char night[3];  // 保存夜间底面RGB的设备字节，24和32位布局都使用这三个颜色字节。
        memcpy(night, sample, sizeof(night));  // 独立记录新会话首帧，不借用会被下一帧覆盖的像素。
        type(&c, "theme day");  // 命令必须通过新console的扩展改变同一桌面主题。
        assert(airi_console_draw(&c));  // 同一console重画，不能重新初始化并丢掉刚设置的主题。
        assert(memcmp(night, sample, sizeof(night)) != 0);  // 日间底栏必须真正换色，只有命令返回不足以通过。
        type(&c, "theme night");  // 沿同一输入会话切回夜间。
        assert(airi_console_draw(&c));  // 再次重画保留本次设置，且不更换底层画布。
        assert(memcmp(night, sample, sizeof(night)) == 0);  // 同一个底面像素恢复首帧字节。
        for (unsigned view = 0; view < 4; ++view) {  // 依次覆盖终端、内存、设备与日志四页。
            airi_console_key(&c, AIRI_KEY_F1 + (int)view);  // 连续功能键枚举以F1为起点加当前页号。
            assert(airi_console_draw(&c));  // 每个真实命令视图都必须成功绘制。
        }
        type(&c, "mem map");  // 输出同一启动描述中的完整物理地图。
        airi_console_key(&c, AIRI_KEY_PAGE_UP);  // 从当前内存页向前翻页，首页面不能下溢。
        assert(airi_console_draw(&c));     // 每个真实命令视图都必须成功绘制。
        for (unsigned i = 0; i < 32; ++i)  // 检查首尾各32字节的数组界限。
            assert(pixels[i] == 0xa5 && pixels[size + 32 + i] == 0xa5);  // 绘图不得改变画布之外的两个哨兵区。
        for (unsigned row = 0; row < height; ++row)  // 每行分别检查，不只看整个数组两端。
            for (unsigned col = width * (layouts[k] / 8); col < pitch; ++col)  // 从可见行末到真实pitch之间都是不可写填充。
                assert(pixels[32 + row * pitch + col] == 0xa5);  // 按真实pitch定位，确认没有写到行尾填充。
        b.framebuffer.pitch = 1;  // 故意让一行跨度小于可见像素字节数。
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 当前非法布局应被拒绝，不能继续绘图。
        b.framebuffer.pitch = pitch;  // 恢复合法行距，下一故障只损坏高度。
        b.framebuffer.height = UINT64_MAX;  // 乘行距必溢出的高度必须拒绝。
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 当前非法布局应被拒绝，不能继续绘图。
        b.framebuffer.height = (uint64_t)INT32_MAX + 1;   // 总字节跨度仍可表示，但高度已不能转成绘图 int 坐标
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 坐标范围也必须拒绝，不能只检查64位乘法
        b.framebuffer.height = 480;      // 恢复合法高度，再检查颜色布局冲突。
        b.framebuffer.green_shift = 16;  // 绿色与红色位区间重叠，不能接受。
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 当前非法布局应被拒绝，不能继续绘图。
        free(pixels);  // 本轮所有绘图、哨兵和非法描述检查完成后，释放主机数组。
    }
    AiriBootInfo b = fixture();  // 在当前调用栈中取得合法完整副本。
    assert(!airi_dashboard_draw(&b));  // 未提供画布的fixture不应报告图形成功。
}
int main(void) {  // 图形目标单独验证实际绘图实现。
    graphics_tests();  // 检查布局、填充、哨兵和无设备拒绝。
    puts("PASS display guards");  // 所有断言完成后才报告通过。
    return 0;  // 正常退出本轮宿主检查。
}
