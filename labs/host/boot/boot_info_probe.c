#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
#include <string.h>     // 主机观察用 strcmp/strcpy 检查或准备文字，不加入内核链接

static AiriBootInfo info;  // 主机夹具的静态对象初始全零，整个进程期间都存在
static int expect(const char *name, const char *wanted) {  // 验证当前 info，将实际错误与本轮想看到的错误比较
    const char *error = airi_boot_validate(&info);      // 把夹具对象地址传入只读验证，NULL 表示成功
    printf("%s: %s\n", name, error ? error : "valid");  // 按 error 是否非空显示具体错误或 valid
    return wanted ? (!error || strcmp(error, wanted)) : error != NULL;  // 有期望错误时必须实际得到同一文字；期望成功时必须返回 NULL
}
int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    info = (AiriBootInfo){.magic = AIRI_BOOT_MAGIC,  // 复合字面量构造新结构值，指定成员之外的字段都初始化为零
                          .version = AIRI_BOOT_VERSION,  // 使用本接口当前版本，作为合法基线
                          .size = sizeof(info),  // 保存完整对象的字节数，而非指针大小
                          .direct_map_offset = UINT64_C(0xffff800000000000),  // 只供格式检查的高地址偏移，主机夹具不解引用相应虚拟地址
                          .kernel_physical_base = 0x100000,  // 模拟内核位于物理 1 MiB 处
                          .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 模拟高地址链接起点，当前只是整数描述
                          .kernel_size = 0x1000,  // 模拟内核跨度为 4096 字节
                          .region_count = 2};     // 仅声明接下来的两个地图条目有效，其余槽位保持零
    strcpy(info.loader, "host probe");  // 把短名称和 NUL 复制进 32 字节数组，当前字面量一定装得下
    info.regions[0] =  // 给第零条地图项写入下面构造的完整区域值
        (struct airi_memory_region){.base = 0x1000, .length = 0x1000, .kind = AIRI_MEMORY_RESERVED};  // 第一段为 [0x1000,0x2000)，用途保留
    info.regions[1] =  // 给第一条地图项写入下面构造的完整区域值
        (struct airi_memory_region){.base = 0x2000, .length = 0x4000, .kind = AIRI_MEMORY_USABLE};  // 第二段为 [0x2000,0x6000)，与前段相邻并提供可用 RAM
    if (expect("baseline", NULL))  // 合法基线必须返回无错误；返回非零说明观察不符
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    info.version++;  // 只改版本一个条件，制造接口版本不匹配
    if (expect("wrong version", "boot ABI mismatch"))  // 必须得到版本不匹配错误，否则观察失败
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    info.version = AIRI_BOOT_VERSION;  // 恢复版本后再制造下一个错误，避免上一错误遮住下一条件
    info.regions[1].base = 0x1800;     // 让第二段起点落在第一段内，制造重叠
    if (expect("overlap", "memory map overlaps or is unsorted"))  // 验证重叠必须在地图顺序检查处被拒绝
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    info.regions[1].base = 0x2000;  // 恢复相邻起点，再测试零长度错误
    info.regions[1].length = 0;     // 第二段改为空区域，保持其他条件不变
    if (expect("empty range", "memory range overflow or empty range"))  // 验证空区域必须在长度检查处被拒绝
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    puts("PASS lab/boot-info: valid ABI and rejected ranges observed");  // 四组结果均符合才报告这轮夹具观察成功
    return 0;  // 本轮所有结果均符合，向 shell 交回成功状态
}
