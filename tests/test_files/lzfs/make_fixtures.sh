#!/bin/zsh
# lzfs smoke 固件生成。
#
#   payload_src.tjs   解压后的期望文本（生成来源，payload 设置 decodedRan）
#   payload_lz4.tjs   payload_src.tjs 的 LZ4 frame 封装（真正会被解压的文件）
#   truncated_lz4.tjs payload_lz4.tjs 去掉尾部（帧不完整 → raw 透传，不挂死）
#   plain.tjs         非 LZ4 普通文本（经 lzfs:// 原样透传）
#
# 依赖 Homebrew lz4 CLI（现代 LZ4 frame 格式，与插件使用的 LZ4F 同族）。
# 生成后建议自检：
#   lz4 -t   tests/test_files/lzfs/payload_lz4.tjs    # 应通过
#   diff <(lz4 -dc tests/test_files/lzfs/payload_lz4.tjs) \
#        tests/test_files/lzfs/payload_src.tjs        # 应无输出
#   lz4 -t   tests/test_files/lzfs/truncated_lz4.tjs  # 应失败
set -e
dir="${0:A:h}"

{
  printf '// lzfs smoke payload：解压后由 Scripts.execStorage 执行。\n'
  printf 'decodedRan = true;\n'
  # 重复且略有变化的长行：保证帧里出现真正的 LZ4 压缩块，而不是 stored。
  for i in {1..96}; do
    printf '// filler %03d aaaaaaaa bbbbbbbb cccccccc dddddddd eeeeeeee ffffffff\n' "$i"
    printf '// filler %03d 0123456789 0123456789 0123456789 0123456789 01234567\n' "$i"
  done
} > "$dir/payload_src.tjs"

lz4 -c "$dir/payload_src.tjs" > "$dir/payload_lz4.tjs"

# 去掉尾部（end mark、可能的 content checksum 与块尾）：帧不完整。
# GNU stat 用 stat -c%s。
size=$(stat -f%z "$dir/payload_lz4.tjs")
head -c $(( size - 24 )) "$dir/payload_lz4.tjs" > "$dir/truncated_lz4.tjs"

printf '// 普通（非 LZ4）文件：经 lzfs:// 应原样透传。\nplainRan = true;\n' \
  > "$dir/plain.tjs"

print "lzfs fixtures regenerated in $dir"
