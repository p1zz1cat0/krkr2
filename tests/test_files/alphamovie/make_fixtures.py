#!/usr/bin/env python3
"""AlphaMovie 插件测试固件生成器。

依据 docs 中的 .amv 格式说明合成最小可解码文件：
  header 'AJPM' + 量化表 + 'FRAM' 帧块序列。
帧内容只使用 DC 系数（每块一个 DC diff + EOB），量化表全 16，
因此 IDCT 后像素 = 2 * DC累积值 + 128，便于像素级断言。
生成三个文件：
  sample_jpeg.amv    JPEG-alpha 路径 (flags=1, 3 张量化表, 2 帧)
  sample_zlib.amv    zlib-alpha 路径 (flags=2, 2 张量化表, 2 帧)
  sample_garbage.amv 非法文件（负向用例）
"""

import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent

# --- 标准 baseline JPEG Huffman 表 (JPEG Annex K) ---
DC_LUMA = [  # (code, len) per category 0..11
    ("00", 2), ("010", 3), ("011", 3), ("100", 3), ("101", 3), ("110", 3),
    ("1110", 4), ("11110", 5), ("111110", 6), ("1111110", 7),
    ("11111110", 8), ("111111110", 9),
]
DC_CHROMA = [
    ("00", 2), ("01", 2), ("10", 2), ("110", 3), ("1110", 4),
    ("11110", 5), ("111110", 6), ("1111110", 7), ("11111110", 8),
    ("111111110", 9), ("1111111110", 10), ("11111111110", 11),
]
EOB_LUMA = "1010"
EOB_CHROMA = "00"


def dc_bits(diff, table):
    """编码一个 DC diff（预测器差值）。"""
    if diff == 0:
        return table[0][0]
    mag = abs(diff)
    cat = mag.bit_length()
    code = table[cat][0]
    if diff > 0:
        extra = f"{diff:0{cat}b}"
    else:
        extra = f"{diff + (1 << cat) - 1:0{cat}b}"
    return code + extra


def dc_block(diff, table, eob):
    return dc_bits(diff, table) + eob


def stuff(bitstr):
    """位串 → 字节，0xFF 后插入 0x00 (JPEG byte stuffing)。"""
    pad = (8 - len(bitstr) % 8) % 8
    bitstr += "1" * pad  # 与标准编码器一致的填充（解码端到标记即停）
    out = bytearray()
    for i in range(0, len(bitstr), 8):
        b = int(bitstr[i:i + 8], 2)
        out.append(b)
        if b == 0xFF:
            out.append(0x00)
    return bytes(out)


def encode_frame_jpeg(frame_num, left, top, w, h, y_values, a_values,
                      cb_diff=0, cr_diff=0):
    """JPEG-alpha 帧：MCU 块序 Cb, Cr, Y0-3, A0-3 (4 成分, 16x16 = 1 MCU)。

    注意非标准共享预测子：Y 与 A 共享 luma 预测子，解码端按块序连续累积；
    因此 A 的差值必须相对「所有 Y 块解码后的预测子」计算。"""
    bits = ""
    bits += dc_block(cb_diff, DC_CHROMA, EOB_CHROMA)   # Cb
    bits += dc_block(cr_diff, DC_CHROMA, EOB_CHROMA)   # Cr (与 Cb 共享 chroma 预测子)
    pred = 0
    for v in y_values:                                  # Y (luma 预测子)
        bits += dc_block(v - pred, DC_LUMA, EOB_LUMA)
        pred = v
    for v in a_values:                                  # A (继续共享同一 luma 预测子)
        bits += dc_block(v - pred, DC_LUMA, EOB_LUMA)
        pred = v
    entropy = stuff(bits)
    header = b"FRAM" + struct.pack("<IIhhHH", 12 + len(entropy), frame_num,
                                    left, top, w, h)
    return header + entropy


def encode_frame_zlib(frame_num, left, top, w, h, y_values, alpha_plane,
                      cb_diff=0, cr_diff=0):
    """zlib-alpha 帧：alpha(zlib) 在前，颜色熵 (Cb,Cr,Y) 在后。"""
    bits = ""
    bits += dc_block(cb_diff, DC_CHROMA, EOB_CHROMA)
    bits += dc_block(cr_diff, DC_CHROMA, EOB_CHROMA)
    prev = 0
    for v in y_values:
        bits += dc_block(v - prev, DC_LUMA, EOB_LUMA)
        prev = v
    color = stuff(bits)
    alpha_z = zlib.compress(bytes(alpha_plane), 6)
    size = 16 + len(alpha_z) + len(color)
    header = b"FRAM" + struct.pack("<IIhhHHI", size, frame_num, left, top,
                                    w, h, len(alpha_z))
    return header + alpha_z + color


def build_amv(flags, quant_tables, frames):
    header_size = 0x28 + 64 * len(quant_tables)
    # 0x00 magic / 0x04 未知域1 / 0x08 rev / 0x0c headerSize / 0x10 未知域2
    # 0x14 numFrames / 0x18 fpsScale / 0x1c fpsRate / 0x20 w / 0x22 h / 0x24 flags
    header = struct.pack(
        "<4s7IHHI",
        b"AJPM", 0, 0, header_size, 0, len(frames), 30, 1, 16, 16, flags)
    header += b"".join(bytes(t) for t in quant_tables)
    assert len(header) == header_size
    return header + b"".join(frames)


QUANT = bytes([16] * 64)

# ---- JPEG-alpha: 2 帧 16x16 ----
# 帧0: Y 块 [0,2,-2,4] A 块 [0,8,4,-4] → 像素 Y=128/132/124/136, A=128/144/136/120
# 帧1: Y 全 0, A 全 2 → 验证跨帧预测子重置 (帧0 末 A 预测子=-4)
f0 = encode_frame_jpeg(0, -3, 2, 16, 16,
                       [0, 2, -2, 4], [0, 8, 4, -4])
f1 = encode_frame_jpeg(1, 4, -2, 16, 16,
                       [0, 0, 0, 0], [2, 2, 2, 2])
(OUT / "sample_jpeg.amv").write_bytes(
    build_amv(1, [QUANT, QUANT, QUANT], [f0, f1]))

# ---- zlib-alpha: 2 帧 16x16 ----
# 帧0: Y [0,-2,2,0] → 128/124/132/124；alpha = 50+8x+2y
# 帧1: Y 全 0 → 128；alpha = 200-8x-2y
w = h = 16
alpha0 = [50 + 8 * x + 2 * y for y in range(h) for x in range(w)]
alpha1 = [200 - 8 * x - 2 * y for y in range(h) for x in range(w)]
z0 = encode_frame_zlib(0, -1, 3, 16, 16, [0, -2, 2, 0], alpha0)
z1 = encode_frame_zlib(1, 2, -4, 16, 16, [0, 0, 0, 0], alpha1)
(OUT / "sample_zlib.amv").write_bytes(
    build_amv(2, [QUANT, QUANT], [z0, z1]))

# ---- 非法文件 ----
(OUT / "sample_garbage.amv").write_bytes(b"this is not an amv file")

print("fixtures written")
