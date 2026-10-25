# 用既有make_command取得交叉工具参数；统一airios入口在K13加入。
# 内核C编译器由调用参数覆盖；此处默认clang，不借宿主libc。
CC := clang
# ELF链接器默认LLD，调用参数可传入真实找到的路径。
LD := ld.lld
# 主机实验用宿主编译器，可以链接宿主libc。
HOST_CC := cc
# ?=仅在调用者未赋值时取normal，各测试配置独立构建。
PROFILE ?= normal
# $(PROFILE)展开当前配置名称，OUT指向这一配置的产物目录。
OUT := build/$(PROFILE)
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/fpu.c kernel/arch/x86_64/tls.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
# guest 图形适配与纯 C 前端进入同一内核，参考服务和 Cocoa 不进入。
SOURCES += kernel/console/display.c $(UI_SOURCES)
else # 没有desktop-前缀时只选择串口实现。
# 串口实现没有字体、绘图与桌面状态。
SOURCES += kernel/console/serial_display.c
endif # 结束当前可选图形分支。
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/fpu_seed.o $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 预处理器从自有include、固定协议头third_party和前端include取得声明。
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -DAIRI_GRAPHICAL_CONSOLE -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifneq ($(filter no-framebuffer service-no-framebuffer,$(CASE)),)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 服务目标只改变启动策略，与旧九种诊断目标使用独立产物目录。
ifneq ($(filter service service-no-framebuffer service-init-failure,$(CASE)),)
CPPFLAGS += -DAIRI_BOOT_SERVICE # 初始化完成后直接进入用户init，不运行教学演示。
endif # 普通配置继续原诊断路径。
ifeq ($(CASE),service-init-failure)
CPPFLAGS += -DAIRI_TEST_INIT_FAILURE # 只在此故障镜像把初始程序换成截断ELF。
endif # 不修改正常init或用户资源接口。
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 设备超时故障镜像故意不发布avail.idx，正常镜像不含注入。
ifeq ($(CASE),block-timeout)
CPPFLAGS += -DAIRI_TEST_BLOCK_TIMEOUT # 验证真实PIT截止与DMA隔离，不冒充设备断电。
endif # 结束块设备故障配置。
# 设备完成存在但故意不交付IRQ，验证PIT驱动的used环后备收集。
ifeq ($(CASE),block-irq-masked)
CPPFLAGS += -DAIRI_TEST_BLOCK_IRQ_MASKED # 不伪造完成，仍使用真实DMA与used.idx。
endif # 其他配置正常开放PCI设备IRQ。
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
endif # 结束栈故障配置。
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
CPPFLAGS += -DAIRI_TEST_TIMER_MASKED # PIT仍计数，但PIC阻止IRQ0到达CPU。
endif # 结束时钟故障条件。
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
CPPFLAGS += -DAIRI_TEST_FATAL_TRAP # 仅此构建目录触发未处理 UD2。
endif # 结束故障配置。
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
# 单引号由shell保护内部双引号，编译器取得C字符串宏值；下一续行同时设置学习日志文字。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束 edit-experiment 的文字覆盖条件

# 这些名称是动作目标，即使同名文件存在也执行其规则。
.PHONY: all kernel image host-test clean
# 默认动作依赖镜像，触发内核编译、链接和镜像制作。
all: image
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c Makefile
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S Makefile
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF依赖全部对象与链接脚本，任一变化会重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
# 这一完整续行命令用ELF x86-64、无标准库、静态链接、4KiB页对齐和不可执行栈；-T指定自有布局，最后-o写目标。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# 镜像先要求独立ELF成功。
image: $(OUT)/airios.elf
# 把同一配置的ELF装入UEFI启动镜像。
	python3 scripts/image.py $(PROFILE)
# 主机字节算法、启动合同与命令状态检查，保留assert并启用ASan/UBSan。
host-test:
# 先建立主机检查产物目录。
	@mkdir -p build/tests
# 以下完整续行命令编译主机检查；fno-builtin避免优化绕过被测实现，sanitizer检查内存和未定义行为，AIRI_HOST_TEST避开真实端口指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/base_test.c \
	    kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c kernel/arch/x86_64/input.c \
		    -o build/tests/base_test
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 独立编译同一物理页算法，主机不解引用 guest 地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/pages_test.c \
	    kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/pages_test
# 验证耗尽、保留范围、重复释放和舍入边界。
	./build/tests/pages_test
# 同一四级页表算法在受控arena里验证，绝不安装宿主页表。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/vm_test.c \
	    kernel/arch/x86_64/paging.c kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/vm_test
# 覆盖翻译、共享分支、空表回收和中途耗尽的所有权回滚。
	./build/tests/vm_test
# 在宿主受控backing上验证同一小对象实现，不借用guest地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/heap_test.c \
	    kernel/mm/heap.c kernel/lib/base.c -o build/tests/heap_test
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 解析器在独立小端字节夹具中核对正常与损坏ELF，不执行输入指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/elf_test.c \
	    kernel/process/elf.c kernel/lib/base.c -o build/tests/elf_test
# 失败不得写出半份view，也不得越界读取映像。
	./build/tests/elf_test
# 文件算法在主机使用同一实现，验证引用、偏移和容量回滚。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/file_test.c \
	    kernel/fs/file.c kernel/lib/base.c -o build/tests/file_test
# 仅全部文件边界成立才继续启动检查。
	./build/tests/file_test
# 日志后端模型验证提交顺序、错误返回与掉电缓存边界。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/journal_test.c \
	    kernel/storage/journal.c kernel/lib/base.c -o build/tests/journal_test
# 模型不声称真实主机掉电，真设备另由guest演示。
	./build/tests/journal_test
# 异步队列用明确延后后端验证SQ/CQ、代数句柄和晚完成。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/async_test.c \
	    kernel/io/engine.c kernel/lib/base.c -o build/tests/async_test
# 全部模型边界通过后才继续真实guest。
	./build/tests/async_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
	python3 -c 'import shutil; shutil.rmtree("build", ignore_errors=True)'
# 后缀替换得到自动依赖文件；前缀-允许首次构建尚无.d。
-include $(OBJECTS:.o=.d)


# 手动选择图形动作时才读取支线目标；默认构建不检查该目录。
ifneq ($(filter ui-preview ui-test lab-editor,$(MAKECMDGOALS)),)
include frontend/targets.mk # 仅显式图形动作读取相应规则。
endif # 结束当前可选图形分支。

# 三个观察名称均是动作，即使有同名文件也运行。
.PHONY: lab-memmove lab-boot-info
# 重新构建重叠移动与截断输出观察。
lab-memmove:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/base/memmove_trace.c kernel/lib/base.c -o build/labs/memmove
# 打印左/右重叠及截断结果。
	./build/labs/memmove
# 重新构建合法/损坏启动数据观察。
lab-boot-info:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/boot/boot_info_probe.c kernel/boot/info.c -o build/labs/boot-info
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -mno-avx -ftls-model=local-exec -MMD -MP -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S Makefile
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户独立静态ELF是嵌入输入，不是内核的直接链接对象。
$(OUT)/user/hello.elf: $(USER_OBJECTS) labs/airios/program.ld
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf $(OUT)/user/waits.elf $(OUT)/user/async.elf Makefile
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -DAIRI_STORAGE_ELF='"$(OUT)/user/storage.elf"' -DAIRI_PIPES_ELF='"$(OUT)/user/pipes.elf"' -DAIRI_WAITS_ELF='"$(OUT)/user/waits.elf"' -DAIRI_ASYNC_ELF='"$(OUT)/user/async.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 多进程工作负载有自己的独立静态ELF。
$(OUT)/user/processes.elf: $(USER_COMMON) $(OUT)/user/processes.o labs/airios/program.ld
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# worker也有自己的_start和main，原始ELF只在包装时进入内核。
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 只链接这份工作负载的main。
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# storage与recover由同一ELF按明确argv分支。
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 管道程序只复用自有用户入口与库。
$(OUT)/user/pipes.elf: $(USER_COMMON) $(OUT)/user/pipes.o labs/airios/program.ld
# 保持相同低地址ELF布局。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# ELF只选择waits的main。
$(OUT)/user/waits.elf: $(USER_COMMON) $(OUT)/user/waits.o labs/airios/program.ld
# 低地址静态链接，不借宿主入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@

# 异步用户程序与重置别名共用独立main和低地址入口。
$(OUT)/user/async.o: labs/airios/async.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 当前配置的对象目录。
	@mkdir -p $(dir $@)
# USER_FLAGS含公开ABI头，不能依赖内核内部结构。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 自有入口、库与async main单独链接。
$(OUT)/user/async.elf: $(USER_COMMON) $(OUT)/user/async.o labs/airios/program.ld
# 仍为无宿主libc的静态ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@

# shell与贯通演示是独立的低地址用户ELF，不链接彼此main。
$(OUT)/user/%.o: labs/airios/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 与既有libc模式规则路径不同，Make按存在的源选择。
	@mkdir -p $(dir $@)
# 复用整数用户ABI，不借宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 两个新程序各自链接自有入口与库。
$(OUT)/user/sh.elf: $(USER_COMMON) $(OUT)/user/sh.o labs/airios/program.ld
# 每个目标包含且只包含自己的main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/sh.o -o $@
# demo同样形成真正ELF，再作为初始装载输入。
$(OUT)/user/demo.elf: $(USER_COMMON) $(OUT)/user/demo.o labs/airios/program.ld
# 段权限与BSS由既有用户链接脚本定义。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/demo.o -o $@
# 目标额外依赖保证.incbin永远读取本配置已完成ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/sh.elf $(OUT)/user/demo.elf
# target-specific变量只对这个包装对象追加路径宏，单引号保护C字符串。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_SHELL_ELF='"$(OUT)/user/sh.elf"' -DAIRI_DEMO_ELF='"$(OUT)/user/demo.elf"'

# 孤儿工作负载复用用户入口和库，保持独立main。
$(OUT)/user/orphans.elf: $(USER_COMMON) $(OUT)/user/orphans.o labs/airios/program.ld
# 独立静态ELF不链接宿主libc。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/orphans.o -o $@
# 包装必须等待此配置自己的ELF完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/orphans.elf
# 汇编只读当前文件路径，不能复用另一配置产物。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_ORPHANS_ELF='"$(OUT)/user/orphans.elf"'

# 动态页工作负载与其他用户main分别链接。
$(OUT)/user/memory.elf: $(USER_COMMON) $(OUT)/user/memory.o labs/airios/program.ld
# 静态ELF只使用自有用户入口与库。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/memory.o -o $@
# 当前包装对象要等本配置的新映像完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/memory.elf
# 文件名作为C字符串宏传入预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MEMORY_ELF='"$(OUT)/user/memory.elf"'

# init与shell拥有各自用户main，监督策略在ring3运行。
$(OUT)/user/init.elf: $(USER_COMMON) $(OUT)/user/init.o labs/airios/program.ld
# 仍使用自有入口和整数用户库，尚未引入POSIX运行时。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/init.o -o $@
# 用户目录包装等本配置init完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/init.elf
# init文件路径作为明确的预处理宏。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_INIT_ELF='"$(OUT)/user/init.elf"'

# 大映像容量观察复用用户库，但保持独立main。
$(OUT)/user/capacity.elf: $(USER_COMMON) $(OUT)/user/capacity.o labs/airios/program.ld
# BSS只占memsz，不能把零填充强塞进内核输入文件。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/capacity.o -o $@
# 包装必须等到本配置容量程序链接完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/capacity.elf
# 原始ELF路径只由此对象的预处理变量消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CAPACITY_ELF='"$(OUT)/user/capacity.elf"'

# 用户SSE/x87观察由独立汇编保持活寄存器，不能用普通函数调用假定XMM保留。
$(OUT)/user/simd-probe.o: labs/airios/simd-probe.S Makefile
# 这个对象只进入用户ELF，与内核状态清理汇编分开。
	@mkdir -p $(dir $@)
# USER_FLAGS开放SSE2但不开放未保存的AVX。
	$(CC) $(USER_FLAGS) -c $< -o $@
# simd的main有真实double表达式；另链接寄存器观察汇编。
$(OUT)/user/simd.elf: $(USER_COMMON) $(OUT)/user/simd.o $(OUT)/user/simd-probe.o labs/airios/program.ld
# 静态链接仍只有一份main，不借宿主浮点运行库。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/simd.o $(OUT)/user/simd-probe.o -o $@
# reset探针在任何编译器SSE指令之前快照入口状态，因此此main保持general-only。
$(OUT)/user/simd-reset.o: labs/airios/simd-reset.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 与普通用户源使用同一配置目录。
	@mkdir -p $(dir $@)
# 后面的general-only只限制此观察C文件，内嵌FXSAVE64仍明确执行。
	$(CC) $(USER_FLAGS) -mgeneral-regs-only -Ilabs/airios/include -c $< -o $@
# 新ELF用于观察exec重新建立默认CPU状态。
$(OUT)/user/simd-reset.elf: $(USER_COMMON) $(OUT)/user/simd-reset.o labs/airios/program.ld
# 用户入口尚不触碰XMM，main第一件事保存真实状态。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/simd-reset.o -o $@
# 包装只等待本配置两份真实用户程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/simd.elf $(OUT)/user/simd-reset.elf
# 文件名以C字符串宏交给预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_SIMD_ELF='"$(OUT)/user/simd.elf"' -DAIRI_SIMD_RESET_ELF='"$(OUT)/user/simd-reset.elf"'

# 用户C与汇编也生成传递头文件依赖，Makefile变化会重编各对象，避免保留旧SSE编译策略。
-include $(wildcard $(OUT)/user/*.d)

# 两份FS观察用户程序分别保留自己的main，仍共用静态用户运行入口。
$(OUT)/user/tls.elf: $(USER_COMMON) $(OUT)/user/tls.o labs/airios/program.ld
# 普通用户C已允许SSE2，本探针的FS访问由明确内嵌汇编执行。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls.o -o $@
# reset只观察exec后FS默认值，不借用旧本地块。
$(OUT)/user/tls-reset.elf: $(USER_COMMON) $(OUT)/user/tls-reset.o labs/airios/program.ld
# 新ELF不与tls的main同时链接。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls-reset.o -o $@
# 包装只等待本配置的两份用户ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/tls.elf $(OUT)/user/tls-reset.elf
# 文件路径通过预处理宏进入.incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_TLS_ELF='"$(OUT)/user/tls.elf"' -DAIRI_TLS_RESET_ELF='"$(OUT)/user/tls-reset.elf"'

# 三份静态TLS用户映像保持各自main，复用普通C入口和自有库。
$(OUT)/user/ctls.elf: $(USER_COMMON) $(OUT)/user/ctls.o labs/airios/program.ld
# local-exec重定位由LLD按PT_TLS静态跨度解析，内核不运行动态链接器。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/ctls.o -o $@
# 单字节模板验证小于TCB对齐时的数据落点。
$(OUT)/user/ctls-char.elf: $(USER_COMMON) $(OUT)/user/ctls-char.o labs/airios/program.ld
# 不能与ctls.o一起链接，否则模板会混入大对齐变量。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/ctls-char.o -o $@
# 只编译超限样本，装载器应在用户运行前返回-8。
$(OUT)/user/tls-too-big.elf: $(USER_COMMON) $(OUT)/user/tls-too-big.o labs/airios/program.ld
# 完整ELF用于真实guest失败事务，不用宿主字符串模拟拒绝。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls-too-big.o -o $@
# 包装等待三份实际产物。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls.elf $(OUT)/user/ctls-char.elf $(OUT)/user/tls-too-big.elf
# 路径只供当前配置的.incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_ELF='"$(OUT)/user/ctls.elf"' -DAIRI_CTLS_CHAR_ELF='"$(OUT)/user/ctls-char.elf"' -DAIRI_TLS_BIG_ELF='"$(OUT)/user/tls-too-big.elf"'

# 四KiB纯零TLS需要第二页TCB，另立ELF不混入其他TLS初值。
$(OUT)/user/ctls-page.elf: $(USER_COMMON) $(OUT)/user/ctls-page.o labs/airios/program.ld
# 与其他用户相同入口和local-exec模型。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/ctls-page.o -o $@
# 包装等待本配置最大模板映像。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls-page.elf
# 单独路径宏只由包装汇编使用。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_PAGE_ELF='"$(OUT)/user/ctls-page.elf"'

# POSIX风格包装是显式用户库，不让早期原始FS和4KiB模板实验强行加入errno。
$(OUT)/user/posix.o: labs/airios/libc/posix.c Makefile
# 使用同一用户目标与自动头依赖，拒绝宿主头文件实现混入。
	@mkdir -p $(dir $@)
# posix.c的_Thread_local错误槽由静态模板装载，库不自己设置FS。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 此程序单独选择posix.o，USER_COMMON仍只包含原始入口和字节/系统调用库。
$(OUT)/user/posix-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/posix-probe.o labs/airios/program.ld
# 无宿主libc，所有标准名字由本项目用户库提供。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/posix-probe.o -o $@
# 包装等待当前配置的完整用户ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/posix-probe.elf
# 该路径宏不改变内核调用号或符号。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_POSIX_ELF='"$(OUT)/user/posix-probe.elf"'

# 向量程序选择相同POSIX错误转换，内核仍使用独立固定宽度描述。
$(OUT)/user/vector-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/vector-probe.o labs/airios/program.ld
# 一份用户main与自有库，不调用宿主readv实现。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/vector-probe.o -o $@
# 包装等待当前配置的实际向量程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/vector-probe.elf
# 文件路径以C字符串宏交给预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_VECTOR_ELF='"$(OUT)/user/vector-probe.elf"'

# 纯块链算法再次以用户选项编译，内核heap.o不进入用户ELF。
$(OUT)/user/heap-algorithm.o: kernel/mm/heap.c Makefile
# 独立输出路径隔离两种编译策略。
	@mkdir -p $(dir $@)
# 算法只需要目标类型，隐式字节复制由用户bytes.o提供。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 对象包装单独选择errno与原始页来源，不改变早期无TLS程序。
$(OUT)/user/malloc.o: labs/airios/libc/malloc.c Makefile
# 与其他用户对象使用同一输出目录和传递头依赖。
	@mkdir -p $(dir $@)
# 所有状态位于当前用户ELF的BSS，编译器不访问内核memory.c。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 分配器观察是独立main，不把标准malloc塞进USER_COMMON。
$(OUT)/user/malloc-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/malloc-probe.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o labs/airios/program.ld
# 原始库、POSIX错误槽、纯算法与用户对象包装都在这个静态ELF内。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 包装等待真实用户分配程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/malloc-probe.elf
# 本配置产物路径仅作为.incbin输入。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MALLOC_ELF='"$(OUT)/user/malloc-probe.elf"'

# 共享执行库显式链接，不让早期原始程序自动依赖线程入口。
$(OUT)/user/threads-lib.o: labs/airios/libc/threads.c Makefile
# 单独命名，与观察程序threads-probe.o分别保存。
	@mkdir -p $(dir $@)
# 使用当前用户C目标和自动头依赖。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户桥接器的call遵循普通C ABI，工作返回再进入自有内核调用。
$(OUT)/user/thread-entry.o: labs/airios/thread-entry.S Makefile
# 它不属于内核架构入口对象。
	@mkdir -p $(dir $@)
# 用户汇编与C使用同一无红区、SSE2目标配置。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 观察程序选择errno/TLS和共享执行库，不调用宿主pthread。
$(OUT)/user/threads.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/threads-probe.o labs/airios/program.ld
# 一份静态用户ELF，全部自有运行符号在链接时解析。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只包本配置真正链接的共享执行程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/threads.elf
# 路径宏供只读incbin消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_THREADS_ELF='"$(OUT)/user/threads.elf"'

# 单独观察TLS零尾部与较小普通BSS，不需要先实现多个执行者。
$(OUT)/user/ctls-tail.elf: $(USER_COMMON) $(OUT)/user/ctls-tail.o labs/airios/program.ld
# 文件初值来自tdata，tbss运行空间不由普通LOAD长度替代。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始映像等当前配置的完整链接结果。
$(OUT)/labs/airios/images.o: $(OUT)/user/ctls-tail.elf
# 只读incbin路径在包装对象中消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CTLS_TAIL_ELF='"$(OUT)/user/ctls-tail.elf"'

# 私有futex观察选择共享执行库，不链接errno或宿主pthread。
$(OUT)/user/futex.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/futex-probe.o labs/airios/program.ld
# 同一静态链接器和用户段权限仍由既有脚本固定。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 包装对象等待本配置的真实新程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/futex.elf
# incbin只读取本配置文件，不读取宿主程序。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_FUTEX_ELF='"$(OUT)/user/futex.elf"'

# 锁实现使用四字节原子内建与自有线程/等待接口。
$(OUT)/user/mutex-lib.o: labs/airios/libc/mutex.c Makefile
# 不与观察程序mutex-probe.o共用对象名。
	@mkdir -p $(dir $@)
# 编译器必须在当前目标直接生成原子指令。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 该用户程序显式选择锁库，不改变其他早期ELF。
$(OUT)/user/mutex.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/mutex-probe.o labs/airios/program.ld
# 静态链接不借宿主pthread或libatomic。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录等待本配置镜像真正完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex.elf
# 路径仅供incbin打包实际字节。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_ELF='"$(OUT)/user/mutex.elf"'

# 与普通malloc.o使用同一实现，仅这份观察对象在持内部锁时主动yield。
$(OUT)/user/malloc-observe.o: labs/airios/libc/malloc.c Makefile
# 观察专用对象不覆盖普通分配器。
	@mkdir -p $(dir $@)
# 宏只启用已经写在源码内的主动换执行者位置，不借宿主线程模拟。
	$(CC) $(USER_FLAGS) -DAIRI_MALLOC_OBSERVE_YIELD -Ilabs/airios/include -c $< -o $@
# 并发程序需要errno TLS、纯块链、同步包装和共享执行库。
$(OUT)/user/heap-threads.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc-observe.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/malloc-threads.o labs/airios/program.ld
# 一份静态ELF只链接一个malloc版本，不混入宿主分配器。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只消费真正生成的新观察ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/heap-threads.elf
# 路径宏只交给只读incbin包装。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_HEAP_THREADS_ELF='"$(OUT)/user/heap-threads.elf"'

# 身份观察链接自有线程与锁库，PT_TLS由本地运行库anchor显式保留。
$(OUT)/user/identity.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/identity.o labs/airios/program.ld
# 同一ELF供普通名称和exec重置名称，运行头仍由内核逐次发布。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 打包等待实际链接完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/identity.elf
# 本配置文件路径交给只读incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_IDENTITY_ELF='"$(OUT)/user/identity.elf"'

# 与普通mutex-lib.o来自同一源码，观察点只用于受控交错ELF。
$(OUT)/user/mutex-observe.o: labs/airios/libc/mutex.c Makefile
# 不覆盖普通对象，也不把回调加进USER_COMMON。
	@mkdir -p $(dir $@)
# 只有此对象可引用观察程序定义的暂停函数。
	$(CC) $(USER_FLAGS) -DAIRI_MUTEX_OBSERVE -Ilabs/airios/include -c $< -o $@
# 最终ELF只选观察锁版本，避免同名函数重复定义。
$(OUT)/user/mutex-races.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-observe.o $(OUT)/user/mutex-races.o labs/airios/program.ld
# 共用真实用户入口与线程运行库，不借宿主并发。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 镜像只打包已链接的观察程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex-races.elf
# 本配置路径只供incbin消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_RACES_ELF='"$(OUT)/user/mutex-races.elf"'

# 变化通知只链接到显式选择它的用户程序。
$(OUT)/user/event-lib.o: labs/airios/libc/event.c Makefile
# 不与观察程序events.o混名。
	@mkdir -p $(dir $@)
# 使用相同用户目标和传递头依赖。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 真实线程等待沿既有int80边界，不借宿主同步库。
$(OUT)/user/events.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/event-lib.o $(OUT)/user/events.o labs/airios/program.ld
# 独立ELF包含自有通知算法和完整观察入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只借最终链接字节。
$(OUT)/labs/airios/images.o: $(OUT)/user/events.elf
# 当前配置路径交给incbin，串口主线不读取图形。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_EVENTS_ELF='"$(OUT)/user/events.elf"'

# 队列库只依赖自有用户锁和变化通知。
$(OUT)/user/queue-lib.o: labs/airios/libc/queue.c Makefile
# 与观察main的queue-probe.o分别命名。
	@mkdir -p $(dir $@)
# 仍为无宿主libc的静态用户目标。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 有界生产消费使用普通锁，不链接暂停观察版。
$(OUT)/user/queue.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/event-lib.o $(OUT)/user/queue-lib.o $(OUT)/user/queue-probe.o labs/airios/program.ld
# 全部执行者运行在同一个实际用户ELF与共享页表中。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 目录只借完成链接的真实文件。
$(OUT)/labs/airios/images.o: $(OUT)/user/queue.elf
# 路径只交给镜像打包，不读取主机业务结果。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_QUEUE_ELF='"$(OUT)/user/queue.elf"'
