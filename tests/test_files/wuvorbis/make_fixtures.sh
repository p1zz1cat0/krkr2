#!/bin/zsh
# wuvorbis smoke 固件生成：0.5s / 22050Hz / 立体声 Vorbis (.ogg)
# Homebrew ffmpeg 的原生 vorbis 编码器只支持 2 声道，故固件为 stereo。
ffmpeg -y -f lavfi -i "sine=frequency=440:duration=0.5" \
  -ar 22050 -ac 2 -c:a vorbis -strict experimental -q:a 5 \
  "$(dirname "$0")/test.ogg"
