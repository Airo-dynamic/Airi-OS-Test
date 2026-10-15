# C 编译器默认名称；命令行 CC=路径可以覆盖此赋值
CC := clang
# ELF 链接器默认名称，工具脚本会传入实际路径
LD := ld.lld
# 主机观察程序的编译器名称，与内核目标工具分开
HOST_CC := cc
# 变量尚未设置时采用 normal，命令行可选择独立故障目录
PROFILE ?= normal
# 立即将本次 profile 展开为产物目录，防止不同版本共用对象
OUT := build/$(PROFILE)
# 加入自有启动校验和主函数，链接器才能取得新入口调用的定义
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c kernel/boot/info.c kernel/main.c
# 将每个 .c 路径换成构建目录中的 .o，再加汇编入口对象
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 编译 x86-64 ELF，选择 C11、优化及调试信息，并把常见警告作为错误。
# 续行中的 freestanding/builtin/stack-protector 选项选择自有运行环境；pic/pie 关闭地址无关产物。
# 最后一行禁用 red zone/SIMD，采用高地址代码模型，并生成 .d 头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 为预处理器指定自有头和锁定协议头的搜索路径
CPPFLAGS := -Iinclude -Ithird_party
ifeq ($(PROFILE),no-framebuffer) # 当 profile 名称是 no-framebuffer 时加入缺画布故障宏
# 在预处理阶段定义故障宏，让适配层略过可选图形响应
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效
ifeq ($(PROFILE),missing-memory) # 缺地图 profile 采用下一项预处理定义
# 使入口局部地图指针变空，验证必需信息错误路径
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效
ifeq ($(PROFILE),edit-experiment) # 改动展示 profile 采用下面两条文字覆盖定义
# 向编译器传入两个字符串宏；单引号保护里面的双引号，使它们成为 C 字面量。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效

.PHONY: all kernel image host-test clean # 这些名字表示动作而非同名文件，避免同名文件使动作被误判已完成
all: image # 默认目标先取得 image，也就需要完成其 ELF 前置依赖
kernel: $(OUT)/airios.elf # 请求 kernel 动作时只生成本 profile 的 ELF
$(OUT)/%.o: %.c # 模式规则将任意匹配的 C 源文件编译为同路径对象文件
	@mkdir -p $(dir $@) # 自动变量 $@ 是对象路径，dir 取父目录；@ 隐藏命令回显
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ # $< 为第一个依赖源码，-c 只编译，-o 指定对象目标
$(OUT)/%.o: %.S # 预处理汇编使用同一对象目录和编译驱动
	@mkdir -p $(dir $@) # 自动变量 $@ 是对象路径，dir 取父目录；@ 隐藏命令回显
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ # $< 为第一个依赖源码，-c 只编译，-o 指定对象目标
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld # 全部对象和链接脚本变化都会使 ELF 需要重新生成
	# 按 x86-64 ELF 静态链接，不加入宿主启动库；最大页大小采用 4096 字节。
	# 续行再禁用可执行栈，使用当前链接脚本，把全部对象写成此目标 ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
image: $(OUT)/airios.elf # 先有当前 ELF，才执行下一行镜像打包
	python3 scripts/image.py $(PROFILE) # 把 profile 作为独立脚本参数交给打包流程
-include $(OBJECTS:.o=.d) # 替换对象后缀读取头依赖文件；开头 - 允许首次尚无 .d
