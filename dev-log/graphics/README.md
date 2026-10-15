# 图形支线：从主机像素走到 guest 显示

这条支线从 K11 的文本与启动基础出发，在独立主机程序里逐步画出像素、文字、窗口和七个应用，再接真实鼠标与键盘。每个停点都有自己的运行结果；暂停图形后可以继续 K12、K13，随后从原停点恢复。两条路线可以任意交替，G24 才要求同时具有 G23 和 K13。

图形历史保存在 `gui` 分支，从主线00的同一个提交分出。它可以合入任一已发布主线阶段；合并只加入图形文件，实际构建仍需下列K11/K13接口。这些合并约定对应本仓库的参考历史：在共享00祖先的main上合入gui后运行。自己的空目录带敲工程继续按下文写入图形实现，逐步建立相同边界。支线只增长自己的源码和可选构建片段，内核入口、文本模型与根 Makefile 由主线维护。Day/Night、粉色身份、斜切标题和全部应用交互随下面的实现逐步形成。

| 顺序 | 实现与反馈 |
|---|---|
| [G11 像素](11-pixels.md) → [G12 字体](12-fonts.md) → [G13 控件](13-themed-controls.md) | 生成PPM，观察地址、裁剪、字形覆盖与同内容双主题 |
| [G14 内存服务](14-services.md) → [G15 窗口](15-windows.md) → [G16 文档显示](16-files-editor-view.md) | 从文件快照走到Terminal/System/Files/Editor |
| [G17 编辑与保存](17-editing.md) → [G18 应用与设置](18-applications.md) → [G19 曲线](19-series.md) | 修改同一文档，保存失败保留输入，切主题保留工作 |
| [G20 命中与选区](20-pointer-routing.md) → [G21 窗口操作](21-window-actions.md) → [G22 指针反馈](22-pointer-feedback.md) | 先用确定的事件序列观察，再接真实输入 |
| [G23 原生窗口](23-native-window.md) | Cocoa接收真实键鼠；纯C观察程序仍可独立运行 |
| [G24 接入guest](24-guest-display.md) | 额外需要K13；显示真实启动信息与文本会话 |
| [G26 图形回归](26-theme-validation.md) | 七个应用、双主题、失败状态与画布边界 |

G11–G23 的反馈来自主机图片或原生窗口，K11–K13 的反馈来自串口。G24 增加可选设备显示实现，把两条路线已有的对象接到一起。完成G24后用`./airios run --graphics`显示guest；G26后运行`./airios ui-preview`和`./airios ui-test`。默认run/test仍使用串口。

主机参考文件保存在本进程内存中，Tasks使用参考记录，Run仍报告执行服务未接入。文件系统、用户任务和持久性由内核主线实现后，才通过服务适配接入这些画面。截图中的正常交互不能证明guest已经具有这些服务。

如果只想先看完整成果，在参考工程运行ui-preview即可；自己的学习目录继续按本集合逐步写出实现。[视觉规范](../../docs/design.md)保留最终设计，[路线入口](https://github.com/Airo-dynamic/Airi-OS-Test/blob/main/dev-log/README.md)记录如何组合或暂停。
