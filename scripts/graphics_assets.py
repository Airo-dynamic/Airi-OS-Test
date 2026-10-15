"""Validate explicitly selected graphics assets using only the Python standard library."""  # 不参与默认串口路径。
import hashlib  # 比较实际字节和锁定摘要。
import json     # 读取已提供的字体身份清单。
from tools import ROOT  # 图形目标与内核使用同一工程根。


def verify_fonts():  # 仅显式选择图形时检查，不导入字体生成器或Cocoa。
    manifest = json.loads((ROOT / 'frontend/assets/fonts.lock.json').read_text())  # 每项含文件名与SHA-256。
    for item in manifest['assets']:  # 遍历字体资源，不扫描任意用户文件。
        actual = hashlib.sha256((ROOT / 'frontend/assets' / item['file']).read_bytes()).hexdigest()  # 摘要来自实际资产。
        if actual != item['sha256']:  # 字节身份不同则拒绝使用。
            raise RuntimeError(f'Font asset checksum mismatch: {item["file"]}')  # 报告具体字体。
    lock = json.loads((ROOT / 'config/deps.lock.json').read_text())['frontend_fonts']  # 总锁记录生成结果身份。
    atlas = ROOT / 'frontend/assets/font-atlas.h'  # 图集由G12配方生成，不手敲数组。
    if hashlib.sha256(atlas.read_bytes()).hexdigest() != lock['atlas_sha256']:  # 同时核对产物而不重新生成。
        raise RuntimeError('Generated font atlas checksum mismatch; rebuild it deliberately')  # 依赖更新必须明确进行。
