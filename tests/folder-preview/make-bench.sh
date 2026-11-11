#!/usr/bin/env bash
# Benchmark tree generator (PLAN.md §6.5 Phase 6): one parent with N subfolders (default 500), each with MIN–MAX
# (default 50–2000) mixed files. Deterministic (seeded), so runs are comparable.
#
# Usage: tests/folder-preview/make-bench.sh [DIR] [--folders N] [--min N] [--max N] [--seed N]
#        DIR defaults to ~/.cache/folder-preview-bench (regenerable; delete it when done).
#
# Files are hard links to a small pool of real media, so ~500 000 entries cost almost no disk space and are
# created in seconds. Mix per folder (by weight): images in several aspect ratios 35 %, videos 8 %,
# audio with cover art 5 %, audio without art 5 % (no thumbnail → skipped), corrupt "images" 4 %
# (thumbnailer fails → skipped), non-media (txt/pdf/zip/odt) 43 %. Hidden and backup files sprinkled in.
# Needs: magick, ffmpeg, python3 (only this script; the extension is C).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="${HOME}/.cache/folder-preview-bench"
FOLDERS=500 MIN=50 MAX=2000 SEED=1
while [ $# -gt 0 ]; do
  case "$1" in
    --folders) FOLDERS="$2"; shift 2 ;;
    --min) MIN="$2"; shift 2 ;;
    --max) MAX="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) DIR="$1"; shift ;;
  esac
done

PARENT="$DIR/parent"
POOL="$DIR/pool"
if [ -e "$PARENT" ]; then
  echo "$PARENT exists; delete $DIR first to regenerate" >&2
  exit 1
fi
mkdir -p "$POOL" "$PARENT"

echo "Building media pool in $POOL ..."
colors=(crimson darkorange gold seagreen teal royalblue purple deeppink sienna olive slategray tomato)
sizes=(320x200 200x300 256x256 400x225 180x270 300x200 240x240 360x160)
for k in $(seq 0 23); do
  magick -size "${sizes[$((k % 8))]}" "gradient:${colors[$((k % 12))]}-white" \
         -gravity center -pointsize 36 -annotate 0 "$k" "$POOL/img$k.$([ $((k % 2)) = 0 ] && echo jpg || echo png)" &
done
wait
cp "$ROOT/tests/folder-preview/data/clip.mp4" "$POOL/vid0.mp4"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=duration=1:size=320x180:rate=5 -pix_fmt yuv420p "$POOL/vid1.mp4"
ffmpeg -loglevel error -y -f lavfi -i mandelbrot=size=240x240:rate=5 -t 1 -pix_fmt yuv420p "$POOL/vid2.mp4"
cp "$ROOT/tests/folder-preview/data/art.mp3" "$POOL/art0.mp3"
cp "$ROOT/tests/folder-preview/data/art.flac" "$POOL/art1.flac"
cp "$ROOT/tests/folder-preview/data/noart.mp3" "$POOL/noart0.mp3"
cp "$ROOT/tests/folder-preview/data/noart.m4a" "$POOL/noart1.m4a"
cp "$ROOT/tests/folder-preview/data/broken.png" "$POOL/broken.png"
cp "$ROOT/tests/folder-preview/data/notes.txt" "$POOL/doc.txt"

# btrfs allows at most 65 535 links per file, so link from 10 replicas of the pool.
for r in $(seq 0 9); do mkdir -p "$POOL/r$r"; cp "$POOL"/*.* "$POOL/r$r/"; done

echo "Creating $FOLDERS folders × $MIN–$MAX files in $PARENT (hard links) ..."
python3 -I - "$POOL" "$PARENT" "$FOLDERS" "$MIN" "$MAX" "$SEED" <<'EOF'
import os, random, sys
pool, parent, folders, lo, hi, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
rnd = random.Random(seed)
imgs = sorted(f for f in os.listdir(pool) if f.startswith("img") and "." in f)
vids = sorted(f for f in os.listdir(pool) if f.startswith("vid") and "." in f)
art = ["art0.mp3", "art1.flac"]; noart = ["noart0.mp3", "noart1.m4a"]
kinds = [("img", 35), ("vid", 8), ("art", 5), ("noart", 5), ("broken", 4), ("doc", 43)]
doc_ext = ["txt", "pdf", "zip", "odt"]
total = 0
for i in range(1, folders + 1):
    d = os.path.join(parent, f"folder{i:04d}")
    os.mkdir(d)
    n = rnd.randint(lo, hi)
    for j in range(n):
        k = rnd.choices([k for k, _ in kinds], weights=[w for _, w in kinds])[0]
        if k == "img":
            src = rnd.choice(imgs); ext = src.rsplit(".", 1)[1]
        elif k == "vid":
            src = rnd.choice(vids); ext = "mp4"
        elif k == "art":
            src = rnd.choice(art); ext = src.rsplit(".", 1)[1]
        elif k == "noart":
            src = rnd.choice(noart); ext = src.rsplit(".", 1)[1]
        elif k == "broken":
            src = "broken.png"; ext = "png"
        else:
            src = "doc.txt"; ext = rnd.choice(doc_ext)
        name = f"{rnd.choice(['IMG_', 'DSC', 'file', 'Scan ', 'clip', 'track'])}{j:05d}.{ext}"
        if rnd.random() < 0.01:
            name = "." + name          # hidden
        elif rnd.random() < 0.01:
            name = name + "~"          # backup
        replica = os.path.join(pool, f"r{(i - 1) * 10 // folders}")
        os.link(os.path.join(replica, src), os.path.join(d, name))
        total += 1
print(f"{folders} folders, {total} files")
EOF
du -sh "$DIR" | sed 's/^/disk use: /'
