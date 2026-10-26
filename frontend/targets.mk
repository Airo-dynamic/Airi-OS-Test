# 图形支线自己的构建目标；默认构建不读取此片段。
include frontend/sources.mk
# 原生窗口与共享C检查均为动作目标。
.PHONY: ui-preview ui-test
# 链接宿主C/Objective-C与Cocoa；这个目标仅macOS可用。
ui-preview:
# 确保原生窗口和前端检查输出目录存在。
	@mkdir -p build/ui
# 下面续行用共享C源码及reference服务，最终链接Cocoa输出宿主窗口程序。
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include \
	    $(UI_SOURCES) frontend/backends/reference.c kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c frontend/platform/macos/preview.m -framework Cocoa -o build/ui/airios-ui
# 编译共享C状态与像素边界检查，不需要Cocoa。
ui-test:
# 确保原生窗口和前端检查输出目录存在。
	@mkdir -p build/ui
# 下面续行编译所有共享源码与参考服务，ASan/UBSan检查该主机进程。
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
	    -Iinclude -Ifrontend/include tests/unit/ui_test.c $(UI_SOURCES) frontend/backends/reference.c \
	    kernel/lib/base.c kernel/boot/info.c kernel/console/console.c -o build/ui/ui_test
# 运行前端动作序列与渲染哨兵检查。
	./build/ui/ui_test
# 在主机中链接 guest 显示适配代码，检查像素边界。
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude -Ifrontend/include tests/unit/display_test.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c kernel/console/display.c $(UI_SOURCES) -o build/ui/display_test
	./build/ui/display_test # 执行当前带哨兵画布检查，失败中止Make。


.PHONY: lab-editor # 同名文件不应使观察动作被跳过。
# 重新构建编辑、保存与执行失败的状态观察。
lab-editor:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include \
	    labs/host/frontend/editor_transaction.c $(UI_SOURCES) frontend/backends/reference.c \
	    kernel/lib/base.c kernel/boot/info.c kernel/console/console.c -o build/labs/editor
# 打印同一文档编辑、保存及Run失败的实际状态。
	./build/labs/editor
