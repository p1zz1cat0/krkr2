#!/bin/zsh
# Make multitrack.opus: 4 channels (2 stereo tracks) in one Ogg Opus stream
# with channel mapping family 255, 2 seconds, 440 Hz tone.
set -eu
cd "${0:A:h}"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=f=440:d=2" \
  -filter_complex "[0]asplit=4[a][b][c][d];[a][b][c][d]amerge=inputs=4" \
  -c:a libopus -mapping_family 255 -b:a 48k multitrack.opus
