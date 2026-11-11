#!/usr/bin/env bash
# Regenerates the small media fixtures in tests/folder-preview/data/ (committed, so tests don't need ffmpeg).
# Needs: ffmpeg, magick (ImageMagick), python3.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

magick -size 64x48 xc:'#d03030' img2.png
magick -size 64x48 xc:'#30a030' img10.png
magick -size 64x48 xc:'#3030d0' photo.jpg
printf 'this is not a png\n' > broken.png
printf 'hello\n' > notes.txt

ffmpeg -y -loglevel error -f lavfi -i testsrc=duration=1:size=160x120:rate=5 -pix_fmt yuv420p clip.mp4

# Audio: 0.3 s mono tone; "art" variants carry a 32×32 cover in each format's native way.
magick -size 32x32 xc:orange "$work/cover.jpg"
ffmpeg -y -loglevel error -f lavfi -i sine=duration=0.3:sample_rate=8000 -ac 1 -c:a libmp3lame noart.mp3
ffmpeg -y -loglevel error -i noart.mp3 -i "$work/cover.jpg" -map 0 -map 1 -c copy -id3v2_version 3 \
       -metadata:s:v comment="Cover (front)" art.mp3
ffmpeg -y -loglevel error -f lavfi -i sine=duration=0.3:sample_rate=8000 -ac 1 -c:a flac "$work/base.flac"
ffmpeg -y -loglevel error -i "$work/base.flac" -i "$work/cover.jpg" -map 0 -map 1 -c copy \
       -disposition:v attached_pic art.flac
ffmpeg -y -loglevel error -f lavfi -i sine=duration=0.3:sample_rate=8000 -ac 1 -c:a aac noart.m4a

ls -la
