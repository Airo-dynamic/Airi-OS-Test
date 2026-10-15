# AiriOS

AiriOS 从自己的 x86-64 内核进入 Linux 存储与 I/O 机制，服务于理解和开发 BOSS、Taishan 这类系统。教程从空目录逐步写出内核、构建、验证与平台适配。内核和 Linux 对照是必修主线，图形工作站在独立的 gui 分支中推进。

从[教程入口](dev-log/README.md)继续。已到 day2 可以接 [03](dev-log/core/03-bytes.md)：00–02 的实现字节保持不变，正文只作必要澄清。内核在 K13 已能独立启动、收发串口命令并验证启动故障；异常、页管理、用户态、文件与异步 I/O 是后续待实现内容。

## 默认从串口运行

以下命令从 main 的完整参考工程执行；自己的带敲目录到 K13 时提供相同入口。

```sh
./airios doctor
./airios build
./airios run
./airios test
```

run 把 QEMU 的 UART 接到当前终端，可以输入 help、mem、mem map、devices、log 和 echo。按 Ctrl-A 后按 X 退出 QEMU。test 检查基础库、启动合同、四种带超时的启动，以及真实 UART/PS/2 输入。

主机需要 Python 3.12+、LLVM/LLD、GNU Make、QEMU、xorriso 和 mtools。macOS 可执行 `brew install llvm lld qemu xorriso mtools make`。虚拟机固定为 q35、单 CPU、256 MiB 和 OVMF UEFI，Apple Silicon 使用 TCG。工具选择、外部来源与依赖校验由教程 00–02 展开。

## 两条分支各自推进

| 分支 | 检查点 | 内容与运行条件 |
| --- | --- | --- |
| main | 18 个：00–10、K11–K13、L01、T01–T03 | 串口内核、必修 Linux 对照和作者工具；不包含图形课程、前端源码或图形资产 |
| gui | 15 个：G11–G24、G26 | 从 main 的 00 根提交分叉，只增加图形自己的路径；合入任一现有 main 阶段均不覆盖主线文件 |

合入源码与开始运行是两件事。图形代码可以先合入较早的 main 检查点，运行需主线到 K11；G24 的 guest 接入需 K13。G11–G23 先提供宿主观察，G26 提供完整图形检查。默认构建继续选择串口实现，不读取字体，也不链接 Cocoa。

在本仓库的参考main历史中，合入GUI使用下面两条命令。自己的空目录带敲工程继续按G11正文写入图形代码；这组参考Git命令要求共享本仓库的00祖先。

```sh
git fetch origin gui # 取回gui及其共同的00祖先。
git merge origin/gui # 只加入支线拥有的图形实现与资料。
```

[图形课程入口](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/README.md)和[视觉设计](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/docs/design.md)位于 gui 分支。图形可暂停在任一检查点，继续内核无需推进它。G24 提供 guest 接入与资产校验后可以选择图形运行，G26 再提供统一的预览与图形检查工具：

```sh
./airios run --graphics # 完成 G24 后运行 guest 图形。
./airios ui-preview # 完成 G26 后打开宿主参考窗口。
./airios ui-test # 完成 G26 后检查图形模型与宿主画面。
./airios test --graphics # 完成 G26 后执行完整图形回归。
```

ui-preview 是 macOS 原生参考窗口。Files/Editor 使用参考进程的会话内存文件，退出后重置；Run 报告执行服务未接入。guest 图形读取真实启动信息和文本控制台，F1–F4 切换视图，theme day/theme night 由图形消费者处理。串口命令没有主题功能。

## Linux 对照与检查点导出

```sh
./airios linux-lab
./airios kernel-debug
./airios learn list
./airios learn export K13 /tmp/airios-core-reference
./airios snapshots
./airios docs-check
```

Linux/GDB 默认只连接个人主机 airi@ssh.airi-dynamic.top。运行证据与连接限制见[验证记录](docs/verification.md)。learn 从参考工程执行，默认只读 main 的 18 个检查点；导出以教程和明确前置关系重建源码，要求目标目录尚不存在。

合并 gui 后，工具通过独立的 `.codex/teaching/graphics.json` 增加图形节点。此时可以列出图形课程，或把任一主线进度与图形停点导出到新目录：

```sh
./airios learn list --track graphics
./airios learn export K13 /tmp/airios-core-with-g13 --alongside G13
```

组合只应用一次共享基础，图形停点不会把主线文件退回旧版。snapshots 在 HISTORY 中生成独立检查点目录；有图形清单时也生成包含所需主线基础的图形快照。已有导出目录不覆盖，HISTORY 中被编辑的快照会阻止重新生成。个人学习位置单独记录，不推断理解程度。

[架构](docs/architecture.md)解释服务、文本与显示的边界，[系统实践地图](docs/system-practice.md)连接项目场景，[路线](docs/roadmap.md)列出后续内核阶段。[AiriBoot](boot/AiriBoot/README.md)是独立的 UEFI 引导子项目。原始代码采用 MIT 许可，外部来源见 [THIRD_PARTY](THIRD_PARTY.md)。
