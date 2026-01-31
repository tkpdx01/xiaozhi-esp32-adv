#!/usr/bin/env python3
"""
小智 ESP32 Assets 打包工具
用于自定义头像和表情包

使用方法:
1. 准备一个目录，包含:
   - index.json (配置文件)
   - *.png (表情图片，建议 32x32 PNG)

2. 运行: python pack_assets.py <输入目录> <输出文件>
   例如: python pack_assets.py ./my_assets ./assets.bin

3. 烧录到设备:
   esptool.py --chip esp32s3 write_flash 0x710000 assets.bin
   (地址根据分区表中 assets 分区的偏移量确定)
"""

import os
import sys
import json
import struct
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None
    print("警告: PIL 未安装，无法自动获取图片尺寸")
    print("安装: pip install Pillow")


def get_image_size(filepath):
    """获取图片尺寸"""
    if Image is None:
        return 0, 0
    try:
        with Image.open(filepath) as img:
            return img.width, img.height
    except Exception:
        return 0, 0


def calculate_checksum(data):
    """计算校验和 (与固件中的算法一致)"""
    checksum = 0
    for byte in data:
        checksum += byte
    return checksum & 0xFFFF


def pack_assets(input_dir, output_file):
    """打包 assets 目录为 bin 文件"""
    input_path = Path(input_dir)

    # 检查 index.json 是否存在
    index_file = input_path / "index.json"
    if not index_file.exists():
        print(f"错误: 找不到 {index_file}")
        print("请创建 index.json 配置文件")
        return False

    # 读取 index.json 获取文件列表
    with open(index_file, 'r', encoding='utf-8') as f:
        index_data = json.load(f)

    # 收集所有需要打包的文件
    files_to_pack = []

    # 首先添加 index.json
    files_to_pack.append(("index.json", index_file))

    # 从 emoji_collection 获取表情文件
    emoji_collection = index_data.get("emoji_collection", [])
    for emoji in emoji_collection:
        filename = emoji.get("file")
        if filename:
            filepath = input_path / filename
            if filepath.exists():
                files_to_pack.append((filename, filepath))
            else:
                print(f"警告: 找不到表情文件 {filepath}")

    # 检查其他可能的资源文件
    for key in ["text_font", "srmodels"]:
        if key in index_data:
            filename = index_data[key]
            filepath = input_path / filename
            if filepath.exists():
                files_to_pack.append((filename, filepath))

    # 检查背景图片
    skin = index_data.get("skin", {})
    for theme in ["light", "dark"]:
        theme_config = skin.get(theme, {})
        bg_image = theme_config.get("background_image")
        if bg_image:
            filepath = input_path / bg_image
            if filepath.exists():
                files_to_pack.append((bg_image, filepath))

    print(f"找到 {len(files_to_pack)} 个文件需要打包:")
    for name, _ in files_to_pack:
        print(f"  - {name}")

    # 构建文件表和数据区
    file_table = []
    data_section = bytearray()

    for name, filepath in files_to_pack:
        # 读取文件内容
        with open(filepath, 'rb') as f:
            content = f.read()

        # 获取图片尺寸
        width, height = 0, 0
        if name.lower().endswith('.png'):
            width, height = get_image_size(filepath)

        # 添加 "ZZ" 魔数
        asset_data = b'ZZ' + content

        # 记录文件信息
        file_table.append({
            'name': name,
            'size': len(content),  # 不包含 ZZ 魔数
            'offset': len(data_section),
            'width': width,
            'height': height
        })

        # 添加到数据区
        data_section.extend(asset_data)

    # 构建文件表二进制数据
    table_data = bytearray()
    for entry in file_table:
        # asset_name[32]
        name_bytes = entry['name'].encode('utf-8')[:32]
        name_bytes = name_bytes.ljust(32, b'\x00')
        table_data.extend(name_bytes)

        # asset_size (uint32)
        table_data.extend(struct.pack('<I', entry['size']))

        # asset_offset (uint32)
        table_data.extend(struct.pack('<I', entry['offset']))

        # asset_width (uint16)
        table_data.extend(struct.pack('<H', entry['width']))

        # asset_height (uint16)
        table_data.extend(struct.pack('<H', entry['height']))

    # 合并文件表和数据区
    payload = table_data + data_section

    # 计算校验和
    checksum = calculate_checksum(payload)

    # 构建文件头
    header = struct.pack('<III',
        len(file_table),  # stored_files
        checksum,         # stored_chksum
        len(payload)      # stored_len
    )

    # 写入输出文件
    with open(output_file, 'wb') as f:
        f.write(header)
        f.write(payload)

    print(f"\n打包完成!")
    print(f"  文件数量: {len(file_table)}")
    print(f"  校验和: 0x{checksum:04X}")
    print(f"  数据长度: {len(payload)} 字节")
    print(f"  总大小: {len(header) + len(payload)} 字节")
    print(f"  输出文件: {output_file}")

    return True


def unpack_assets(input_file, output_dir):
    """解包 assets.bin 文件"""
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    with open(input_file, 'rb') as f:
        data = f.read()

    # 解析文件头
    stored_files, stored_chksum, stored_len = struct.unpack('<III', data[:12])

    print(f"文件头信息:")
    print(f"  文件数量: {stored_files}")
    print(f"  校验和: 0x{stored_chksum:04X}")
    print(f"  数据长度: {stored_len}")

    # 验证校验和
    payload = data[12:12+stored_len]
    calculated_checksum = calculate_checksum(payload)
    if calculated_checksum != stored_chksum:
        print(f"警告: 校验和不匹配! 计算值: 0x{calculated_checksum:04X}")

    # 解析文件表
    table_size = 44  # sizeof(mmap_assets_table)
    data_offset = 12 + table_size * stored_files

    print(f"\n文件列表:")
    for i in range(stored_files):
        offset = 12 + i * table_size

        # 解析文件表条目
        name_bytes = data[offset:offset+32]
        name = name_bytes.rstrip(b'\x00').decode('utf-8')

        asset_size, asset_offset, width, height = struct.unpack(
            '<IIHH', data[offset+32:offset+44]
        )

        print(f"  [{i+1}] {name}")
        print(f"      大小: {asset_size}, 偏移: {asset_offset}, 尺寸: {width}x{height}")

        # 提取文件内容
        file_offset = data_offset + asset_offset
        magic = data[file_offset:file_offset+2]

        if magic == b'ZZ':
            content = data[file_offset+2:file_offset+2+asset_size]

            # 保存文件
            output_file = output_path / name
            with open(output_file, 'wb') as f:
                f.write(content)
            print(f"      已保存到: {output_file}")
        else:
            print(f"      警告: 魔数不匹配 ({magic})")

    print(f"\n解包完成! 文件保存到: {output_dir}")
    return True


def create_template(output_dir):
    """创建模板目录"""
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    # 创建 index.json 模板
    index_template = {
        "version": 1,
        "chip_model": "esp32s3",
        "hide_subtitle": False,
        "display_config": {
            "width": 240,
            "height": 135,
            "monochrome": False,
            "color": "RGB565"
        },
        "skin": {
            "light": {
                "text_color": "#000000",
                "background_color": "#ffffff"
            },
            "dark": {
                "text_color": "#ffffff",
                "background_color": "#121212"
            }
        },
        "emoji_collection": [
            {"name": "neutral", "file": "neutral.png"},
            {"name": "happy", "file": "happy.png"},
            {"name": "laughing", "file": "laughing.png"},
            {"name": "funny", "file": "funny.png"},
            {"name": "sad", "file": "sad.png"},
            {"name": "angry", "file": "angry.png"},
            {"name": "crying", "file": "crying.png"},
            {"name": "loving", "file": "loving.png"},
            {"name": "embarrassed", "file": "embarrassed.png"},
            {"name": "surprised", "file": "surprised.png"},
            {"name": "shocked", "file": "shocked.png"},
            {"name": "thinking", "file": "thinking.png"},
            {"name": "winking", "file": "winking.png"},
            {"name": "cool", "file": "cool.png"},
            {"name": "relaxed", "file": "relaxed.png"},
            {"name": "delicious", "file": "delicious.png"},
            {"name": "kissy", "file": "kissy.png"},
            {"name": "confident", "file": "confident.png"},
            {"name": "sleepy", "file": "sleepy.png"},
            {"name": "silly", "file": "silly.png"},
            {"name": "confused", "file": "confused.png"}
        ]
    }

    index_file = output_path / "index.json"
    with open(index_file, 'w', encoding='utf-8') as f:
        json.dump(index_template, f, indent=2, ensure_ascii=False)

    print(f"模板已创建: {output_dir}")
    print(f"\n请将以下表情图片放入目录 (建议 32x32 PNG):")
    for emoji in index_template["emoji_collection"]:
        print(f"  - {emoji['file']}")

    print(f"\n可选配置:")
    print(f"  - 修改 skin.light/dark 中的颜色")
    print(f"  - 添加 background_image 设置背景图片")
    print(f"  - 设置 hide_subtitle: true 隐藏字幕")

    return True


def print_usage():
    print("小智 ESP32 Assets 打包工具")
    print()
    print("用法:")
    print("  python pack_assets.py pack <输入目录> <输出文件>")
    print("      打包目录为 assets.bin")
    print()
    print("  python pack_assets.py unpack <输入文件> <输出目录>")
    print("      解包 assets.bin 到目录")
    print()
    print("  python pack_assets.py template <输出目录>")
    print("      创建模板目录")
    print()
    print("示例:")
    print("  python pack_assets.py template ./my_assets")
    print("  python pack_assets.py pack ./my_assets ./assets.bin")
    print("  python pack_assets.py unpack ./assets.bin ./extracted")
    print()
    print("烧录到设备:")
    print("  esptool.py --chip esp32s3 write_flash 0x710000 assets.bin")
    print("  (地址根据分区表确定)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print_usage()
        sys.exit(1)

    command = sys.argv[1].lower()

    if command == "pack" and len(sys.argv) == 4:
        success = pack_assets(sys.argv[2], sys.argv[3])
        sys.exit(0 if success else 1)

    elif command == "unpack" and len(sys.argv) == 4:
        success = unpack_assets(sys.argv[2], sys.argv[3])
        sys.exit(0 if success else 1)

    elif command == "template" and len(sys.argv) == 3:
        success = create_template(sys.argv[2])
        sys.exit(0 if success else 1)

    else:
        print_usage()
        sys.exit(1)
