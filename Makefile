# 交叉编译C和汇编的LLVM驱动
CC := clang
# 把独立ELF对象链接成内核
LD := ld.lld
# 后续主机观察使用本机C编译器
HOST_CC := cc
# 当前构建变体，未指定时normal
PROFILE ?= normal
# 当前变体产物目录
OUT := build/$(PROFILE)
# 已接入的内核C文件清单
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c kernel/boot/info.c kernel/main.c kernel/console/console.c kernel/arch/x86_64/input.c
# 每个C文件映射到对象路径，再加入汇编入口对象
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 独立x86-64 ELF、C11和无宿主运行时等编译约束
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 自有接口和协议头的包含目录
CPPFLAGS := -Iinclude -Ithird_party
# 按PROFILE构建变体选择下面的故障或输出宏
ifeq ($(PROFILE),no-framebuffer)
# 在已有预处理参数后追加当前变体宏
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
# 结束本次PROFILE条件分支
endif
# 按PROFILE构建变体选择下面的故障或输出宏
ifeq ($(PROFILE),missing-memory)
# 在已有预处理参数后追加当前变体宏
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
# 结束本次PROFILE条件分支
endif
# 按PROFILE构建变体选择下面的故障或输出宏
ifeq ($(PROFILE),edit-experiment)
# 在已有预处理参数后追加当前变体宏
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
# 结束本次PROFILE条件分支
endif

# 这些名字是操作目标，即使存在同名文件也不判已完成
.PHONY: all kernel image host-test clean
# all为默认动作，依赖image，自身没有recipe。
all: image
# kernel为显式编译入口，ELF目标决定是否重建。
kernel: $(OUT)/airios.elf
# 声明本目标依赖哪些输入；源文件比对象新时重新执行recipe
$(OUT)/%.o: %.c
	# 先创建目标父目录；@隐藏命令回显，$(dir $@)取目标目录
	@mkdir -p $(dir $@)
	# 分别传预处理与编译参数；$<为首个源前提，$@为当前目标路径
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 声明本目标依赖哪些输入；源文件比对象新时重新执行recipe
$(OUT)/%.o: %.S
	# 先创建目标父目录；@隐藏命令回显，$(dir $@)取目标目录
	@mkdir -p $(dir $@)
	# 分别传预处理与编译参数；$<为首个源前提，$@为当前目标路径
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF任一对象或链接脚本更新时重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
	# 独立静态链接，无宿主libc，链接脚本决定段与入口位置
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# image是.PHONY动作，每次调用都运行镜像制作；依赖先更新ELF。
image: $(OUT)/airios.elf
	# 把当前PROFILE传给镜像脚本，复用已锁定固件与引导器
	python3 scripts/image.py $(PROFILE)
# 加载编译器生成的头文件依赖；缺失.d不会中断第一次构建
-include $(OBJECTS:.o=.d)
