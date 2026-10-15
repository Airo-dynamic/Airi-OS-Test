# 三个名称都是动作目标，不依赖同名文件是否存在。
.PHONY: help status build
# 默认help执行status，显示当前子项目能力。
help: status
# status只打印当前边界，不制造EFI产物。
status:
	@echo "AiriBoot: shared contract ready; A1/A2/A3 implementation pending." # @隐藏命令回显，文字声明尚未实现。
	@echo "Read README.md. AiriOS P1 currently boots with Limine." # 当前内核仍由固定Limine启动。
# build尚无装载器实现，必须给非零退出而不是假成功。
build:
	@echo "AiriBoot loader is not implemented yet; no EFI output exists." >&2 # >&2把原因送到标准错误。
	@exit 2 # 失败状态交给调用Make的进程。
