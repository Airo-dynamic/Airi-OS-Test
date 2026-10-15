"""Generate the checked-in A8 atlas from locked fonts with Pillow 12.3.0."""  # 在主机将固定字体转换为guest可读取的A8覆盖率数组
import hashlib  # 读取资产字节后计算SHA-256摘要
import json     # 导入JSON解析，将锁文件文本转成程序内字典
from pathlib import Path  # 导入Path，后续用路径对象组合观察输出位置
from PIL import Image, ImageDraw, ImageFont  # 借用Pillow创建灰度图片、画字形和取得字体度量

ROOT = Path(__file__).resolve().parents[1]  # 本脚本目录的上一级就是学习仓库根目录
assets = ROOT / 'frontend/assets'  # Path的/按路径段连接，得到字体与生成图集所在目录
lock = json.loads((assets / 'fonts.lock.json').read_text())  # 先读取UTF-8文本，再把assets键对应的数组变成记录列表
for record in lock['assets']:  # 每轮record是一个含file、url和sha256的资产字典
    actual = hashlib.sha256((assets / record['file']).read_bytes()).hexdigest()  # 读当前资产原始字节并取得十六进制摘要字符串
    if actual != record['sha256']:  # 摘要不等于锁定值时不能继续生成可信图集
        raise RuntimeError(f'Font checksum mismatch: {record["file"]}')  # f字符串带入失败文件名，异常使生成在写输出前停止

faces = [('ibmplexsans.ttf', 12), ('ibmplexsans.ttf', 14), ('ibmplexsans.ttf', 16),  # 列表的每项是字体文件名与字号二元组，顺序对应C字体枚举
         ('ibmplexsans.ttf', 22), ('ibmplexmono.ttf', 14), ('ibmplexmono.ttf', 16), ('ibmplexsans.ttf', 14), ('ibmplexsans.ttf', 32)]  # 补标题和两套等宽字体，再补14px中等字重与32px轻字重，共八项
data = []     # 大覆盖率数组起初为空，后续逐字节追加0到255整数
metrics = []  # 八套字体的度量列表起初为空，每项将是一整套glyphs
for index, (filename, size) in enumerate(faces):  # enumerate给出下标index与一项元组，内层括号再解包成文件名和字号
    font = ImageFont.truetype(str(assets / filename), size)  # 将Path转字符串交给Pillow，按当前字号读取TTF轮廓
    if filename.startswith('ibmplexsans'):  # 可变Sans保存同一套轮廓的字重与字宽轴
        weight = 400    # 正文和小字先使用常规字重
        if index == 7:  # 第八套用于32px大读数
            weight = 300  # 大字号使用较轻笔画
        elif index in (2, 3, 6):  # 标签、标题和14px按钮使用中等字重
            weight = 500  # 当前三个下标共享此轴值
        font.set_variation_by_axes([weight, 100])  # Pillow按本字体wght、wdth轴顺序取得字重和标准字宽
    glyphs = []  # 当前字体的95个字符度量暂存于新列表
    for code in range(32, 127):  # range含32不含127，覆盖空格到波浪号的95个ASCII编码
        character = chr(code)  # 把整数ASCII编码变为一字符Python字符串
        x0, y0, x1, y1 = font.getbbox(character)  # 四元组解包为当前字形包围盒左上和右下坐标
        width, height = x1 - x0, y1 - y0  # 用两端坐标差得到字形像素尺寸
        bitmap = Image.new('L', (max(width, 1), max(height, 1)))  # L是每像素一字节灰度，空字形也借至少1×1的Pillow图片
        ImageDraw.Draw(bitmap).text((-x0, -y0), character, font=font, fill=255)  # 抵消包围盒偏移后将轮廓画成0到255覆盖率
        glyphs.append((len(data), width, height, x0, y0, round(font.getlength(character))))  # 加入一条六元组，offset取当前覆盖率长度，advance将浮点间距取最近整数
        if width * height:  # 只有实际字形有像素时才把图片内容加入大覆盖率数组
            data.extend(bitmap.tobytes())  # bytes逐项迭代得到整数，extend逐字节加入而非嵌套一块bytes
    metrics.append(glyphs)  # append把整套字符度量列表作为八套字体列表的一项

lines = ['/* Generated from locked OFL fonts by scripts/font_atlas.py. */',  # 开始收集生成C文件的源码行，第一行说明固定字体来源
         'struct u_glyph { unsigned offset; unsigned char w,h; signed char x,y; unsigned char advance; };',  # 输出一条字形记录类型：覆盖率偏移、尺寸、文字偏移和推进间距
         'static const struct u_glyph u_glyphs[8][95] = {']  # 八套字体各95字符的二维度量数组开始
for glyphs in metrics:  # 依字体枚举顺序输出每套95条字符度量
    lines.append('{')     # 生成C二维数组中当前字体这一层的开始括号
    for glyph in glyphs:  # 当前glyph是offset、尺寸、偏移和推进量的六元组
        values = ','.join(str(value) for value in glyph)  # 生成器逐项将整数转字符串，join用逗号组成C初始化字段
        lines.append('{' + values + '},')  # 把本字符度量包在C结构初始化括号中，加尾逗号
    lines.append('},')  # 结束当前字体的95条度量初始化
lines.extend(['};', 'static const unsigned char u_coverage[] = {'])  # 结束度量数组，再打开每元素一字节的覆盖率数组
for offset in range(0, len(data), 32):  # 每32个覆盖率整数形成一行源码，最后一行可少于32项
    values = ','.join(str(value) for value in data[offset:offset + 32])  # 切片取本批前缀，逐个转成十进制C整数文本
    lines.append(values + ',')  # 输出这一批覆盖率，末尾逗号允许下一批继续
lines.append('};')  # 结束覆盖率C数组
(assets / 'font-atlas.h').write_text('\n'.join(lines) + '\n')  # 按换行连接源码各行，再加末尾换行写生成头文件
print('Atlas:', len(data), 'bytes /', len(faces), 'faces')     # 报告真实生成覆盖率字节数和八套字体数量
