"""Build and inspect the shared native frontend without bootloader dependencies."""  # 模块说明文字；构建和窗口启动由下面的函数调用触发
import hashlib   # 摘要比较读取到的真实字库与图集字节。
import json      # 从锁文件恢复字典，报告再编码为JSON。
import platform  # 判断当前操作系统是否提供Cocoa。
import subprocess  # 按参数列表运行Make、原生窗口和图像转换。
import plistlib    # 把应用元数据字典编码成macOS属性列表。
import shutil      # 复制原生可执行文件并保留权限。
from pathlib import Path  # 路径对象支持/拼接，不是字符串除法。
from graphics_assets import verify_fonts  # 与guest共用纯标准库资产校验。
from tools import ROOT, make_command      # 复用根目录和Make工具参数。

APPS = ('terminal', 'files', 'editor', 'tasks', 'system', 'settings', 'viewer')  # 固定顺序对应C应用枚举。

def package_preview():  # 给已有可执行文件建立macOS .app目录结构。
    app = ROOT / 'build/ui/AiriOS Frontend.app'  # .app是包含程序和元数据的目录，不是另一份源码。
    executable = app / 'Contents/MacOS/AiriOSFrontend'     # macOS按元数据从这里找实际可执行文件。
    executable.parent.mkdir(parents=True, exist_ok=True)   # 补齐目录，已有目录不报错。
    shutil.copy2(ROOT / 'build/ui/airios-ui', executable)  # 复制本次编译结果并保留可执行权限。
    metadata = {  # 各键都被macOS应用加载路径读取。
        'CFBundleIdentifier': 'dev.airios.frontend',  # 应用身份字符串，与窗口标题不同。
        'CFBundleName': 'AiriOS',  # 应用内部名称。
        'CFBundleDisplayName': 'AiriOS',  # 面向用户显示的名称。
        'CFBundleExecutable': 'AiriOSFrontend',  # 必须与Contents/MacOS下的文件名一致。
        'CFBundleVersion': '1',  # 本演示包版本字符串。
        'CFBundlePackageType': 'APPL',    # 表示这是应用程序包。
        'NSHighResolutionCapable': True,  # 允许高分屏呈现；画布仍按前章逻辑尺寸分配。
    }
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(metadata))  # 字典转plist字节写入规定路径。
    return app  # 返回包目录，open以它为启动目标。

def preview(app='editor', state='normal'):  # 默认打开Editor普通状态。
    verify_fonts()  # 字体有效才继续构建。
    if platform.system() != 'Darwin':  # Cocoa适配只存在于macOS。
        raise RuntimeError('Native preview uses macOS Cocoa; shared C model tests run without Cocoa')  # 给出平台边界。
    subprocess.run(make_command(target='ui-preview'), cwd=ROOT, check=True)  # 用同一Make规则链接平台适配。
    bundle = package_preview()  # 把刚生成的程序装进.app目录。
    subprocess.run(['open', '-n', '-W', str(bundle), '--args', '--app', app, '--state', state], cwd=ROOT, check=True)  # -n新建实例，-W等待退出，--args后传给程序。

def check_contrast(out):  # 使用已编译程序导出的角色颜色计算实际组合。
    def luminance(rgb):  # sRGB 编码先转换成线性光，再加权求相对亮度。
        total = 0.0  # 黑色的初始亮度为零。
        for shift, weight in ((16, .2126), (8, .7152), (0, .0722)):  # 依红绿蓝位移与亮度权重取通道。
            value = ((rgb >> shift) & 255) / 255  # 八位编码归一化到零至一。
            linear = value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4  # 逆 sRGB 转移函数。
            total += weight * linear  # 累积该通道的线性亮度贡献。
        return total  # 结果用于两个颜色之间的对比。
    pairs = []  # 每项三元组保存前景角色、背景角色和目标比例。
    for foreground in ('TEXT', 'MUTED'):  # 正文与辅助文字各检查相同六个表面。
        for background in ('BG', 'PANEL', 'SURFACE', 'RAISED', 'HOVER', 'PRESSED'):  # 实际出现的内容与交互背景。
            pairs.append((foreground, background, 4.5))  # 普通文字要求至少4.5:1。
    for foreground in ('ACCENT', 'BLUE', 'RED', 'WARM', 'PINK', 'SUCCESS'):  # 六种语义和代码文字颜色。
        pairs.append((foreground, 'SURFACE', 4.5))  # 这些文字位于内容表面。
    for background in ('RAISED', 'PRESSED', 'HOVER'):  # 选中按钮使用的交互底色。
        pairs.append(('ACCENT', background, 4.5))  # 选中前景仍需可读。
    for background in ('PANEL', 'SURFACE', 'BG'):  # 必要控件边界邻接表面。
        pairs.append(('CONTROL', background, 3.0))  # 控件边界使用非文字3:1目标。
    for background in ('PANEL', 'SURFACE', 'RAISED'):  # 焦点框邻接背景。
        pairs.append(('BLUE', background, 3.0))  # 蓝色焦点边界同样使用3:1目标。
    pairs.append(('INK', 'CHROME', 4.5))  # 标题壳体文字与底色。
    pairs.append(('ON_ACCENT', 'ACCENT', 4.5))  # 实色主按钮内部文字与底色。
    results = []  # 记录每个实际检查值，失败时也保留。
    for theme in ('night', 'day'):  # 两个模式分别输出自己的完整调色板。
        raw = subprocess.check_output([str(ROOT / 'build/ui/airios-ui'), '--palette', '--theme', theme], timeout=15)  # 有限等待原生工具 JSON 输出。
        palette = json.loads(raw)     # 解码得到角色名称到 RGB 整数的字典。
        for fg, bg, target in pairs:  # 解包当前文字或边界组合及目标阈值。
            low, high = sorted((luminance(palette[fg]), luminance(palette[bg])))  # 按线性亮度排序，模式深浅不影响公式。
            ratio = (high + .05) / (low + .05)  # WCAG 对比公式，原值比较前不四舍五入。
            results.append(dict(theme=theme, foreground=fg, background=bg, ratio=ratio, target=target, passed=ratio >= target))  # 保存输入角色与实际结论。
    (out / 'contrast.json').write_text(json.dumps(results, indent=2) + '\n')  # 报告与截图保存在同一验证目录。
    failures = [row for row in results if not row['passed']]  # 只汇总未达到阈值的组合。
    if failures:  # 对比失败使总验证失败，不能仅产生报告。
        raise RuntimeError(f'Contrast targets failed: {failures}')  # 显示具体模式、角色、比值与目标。
    print(f'PASS contrast: {len(results)} text/control combinations')  # 仅所有组合成立才报告通过。

def check_frontend(screenshots=True):  # 先跑可移植C行为检查，平台支持时再生成状态图库。
    verify_fonts()  # 检查和显示使用相同字库。
    subprocess.run(make_command(target='ui-test'), cwd=ROOT, check=True)  # C状态与绘图检查可在非macOS运行。
    if not screenshots or platform.system() != 'Darwin':  # 显式禁截图或平台无Cocoa时到此结束。
        return  # 不执行macOS快照和sips。
    subprocess.run(make_command(target='ui-preview'), cwd=ROOT, check=True)  # 重新构建实际截图程序。
    out = ROOT / 'artifacts/frontend'  # 保留本轮图像与JSON结果。
    out.mkdir(parents=True, exist_ok=True)  # 本轮报告和图片共享产物目录。
    for name in ('contrast.json', 'render-tests.json'):  # 两份汇总只能表示本轮完整完成的检查。
        (out / name).unlink(missing_ok=True)  # 本轮若中途失败，不保留旧成功汇总。
    check_contrast(out)  # 先检查实际语义颜色，再生成状态图供目视复核。
    cases = []  # 每项六元组依次为name/app/size/state/scenario/tab。
    for size in ('1024x768', '1280x800', '1600x900'):  # 三种实际布局尺寸。
        for app in APPS:  # 七个应用每种尺寸各生成一张。
            cases.append((f'{app}-{size}', app, size, 'normal', 'none', 0))  # 名称用于文件路径，其余项直接作为程序参数。
    for tab in range(1, 5):  # range上界不含5，补System的页签1..4。
        cases.append((f'system-tab{tab}', 'system', '1280x800', 'normal', 'none', tab))  # 页签0已经由普通应用快照覆盖。
    for tab in range(1, 4):  # 补Settings的页签1..3。
        cases.append((f'settings-tab{tab}', 'settings', '1280x800', 'normal', 'none', tab))  # 保存应用与页签之间的对应。
    for state in ('empty', 'loading', 'error', 'offline'):  # 服务状态分别影响空、等待、失败和离线画面。
        for app in ('files', 'tasks'):  # 这两种列表应用需要分别观察每种状态。
            cases.append((f'{app}-{state}', app, '1280x800', state, 'none', 0))  # 状态名传入真实程序状态分支。
    for scenario in ('dialog', 'dirty', 'workspace', 'save-error', 'system-tasks', 'controls'):  # 单独观察执行失败、未保存与两窗口工作区。
        cases.append((scenario, 'editor', '1280x800', 'normal', scenario, 0))  # 场景名称也成为输出文件名。
    results = []  # 只在快照和转换均成功后加入对应结果。
    for theme in ('night', 'day'):  # 两个模式各自重跑相同42项场景。
        for name, app, size, state, scenario, tab in cases:  # 六元组解包成输出名与原生参数。
            name = theme + '-' + name    # 文件名明确标出模式，两个版本都保留。
            ppm = out / (name + '.ppm')  # 原生程序输出二进制PPM。
            png = out / (name + '.png')  # 转换后便于查看的图像路径。
            command = [  # 使用参数列表，文件名含空格仍是单个参数。
                str(ROOT / 'build/ui/airios-ui'), '--snapshot', str(ppm),  # 同一平台绘图程序直接生成快照后退出。
                '--app', app, '--size', size, '--state', state,  # 确定本次应用、画布和服务状态。
                '--scenario', scenario, '--tab', str(tab), '--theme', theme,  # 整数页签显式转文本交给CLI。
            ]
            subprocess.run(command, cwd=ROOT, check=True, timeout=15)  # 单张快照最多15秒，非零或超时都失败。
            subprocess.run(['sips', '-s', 'format', 'png', str(ppm), '--out', str(png)], check=True, stdout=subprocess.DEVNULL, timeout=15)  # 只隐藏转换正常输出，错误仍可见。
            results.append({'name': name, 'png': str(png.relative_to(ROOT)), 'passed': True})  # 保存可搬迁的项目相对路径。
    (out / 'render-tests.json').write_text(json.dumps(results, indent=2) + '\n')  # 所有结果写成JSON数组。
    print(f'PASS frontend renders: {len(results)} application/layout/state snapshots')  # 按真实完成数量报告双主题样本。
