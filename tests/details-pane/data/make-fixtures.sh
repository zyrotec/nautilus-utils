#!/usr/bin/env bash
# Regenerates the small fixtures in tests/details-pane/data/ (committed, so tests need none of these tools).
# Needs: gst-launch-1.0 (base, good, openh264, libav plugins), exiv2, python3.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# Video: 0.4 s, 64×48 at 10 fps, VP8 + Vorbis in WebM and H.264 + AAC in MP4.
gst-launch-1.0 -q videotestsrc num-buffers=4 ! video/x-raw,width=64,height=48,framerate=10/1 ! vp8enc ! queue ! \
  webmmux name=m ! filesink location=clip.webm \
  audiotestsrc num-buffers=18 ! audio/x-raw,rate=44100,channels=2 ! audioconvert ! vorbisenc ! queue ! m.
gst-launch-1.0 -q videotestsrc num-buffers=4 ! video/x-raw,width=64,height=48,framerate=10/1 ! openh264enc ! \
  h264parse ! queue ! mp4mux name=m ! filesink location=clip.mp4 \
  audiotestsrc num-buffers=18 ! audio/x-raw,rate=44100,channels=1 ! audioconvert ! avenc_aac ! queue ! m.
# Audio: 0.5 s stereo Vorbis with tags.
gst-launch-1.0 -q audiotestsrc num-buffers=22 ! audio/x-raw,rate=44100,channels=2 ! audioconvert ! \
  taginject 'tags="title=(string)\"Test Tone\",artist=(string)\"Nautilus Utils\",album=(string)Fixtures,track-number=(uint)3"' ! \
  vorbisenc ! oggmux ! \
  filesink location=tone.ogg

# Images: a JPEG with camera EXIF, a PNG without.
gst-launch-1.0 -q videotestsrc num-buffers=1 ! video/x-raw,width=120,height=80 ! jpegenc ! filesink location=photo.jpg
exiv2 -M"set Exif.Image.Make Canon" -M"set Exif.Image.Model Canon EOS R6" \
      -M"set Exif.Photo.ISOSpeedRatings 400" -M"set Exif.Photo.FNumber 28/10" \
      -M"set Exif.Photo.ExposureTime 1/250" -M"set Exif.Photo.FocalLength 50/1" \
      -M"set Exif.Photo.DateTimeOriginal 2024:03:09 12:34:56" -M"set Exif.Image.Orientation 1" photo.jpg
gst-launch-1.0 -q videotestsrc num-buffers=1 ! video/x-raw,width=30,height=20 ! pngenc ! filesink location=pic.png
cat > pic.svg <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" width="40" height="25"><rect width="40" height="25" fill="#3584e4"/></svg>
SVG

# PDF: 2 pages (A4, Letter) with an info dictionary.
python3 -I - <<'PY'
objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.28 841.89] >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    b"<< /Title (Fixture Document) /Author (Nautilus Utils) /Subject (Tests) /Keywords (pdf, fixture)"
    b" /Creator (make-fixtures.sh) /Producer (python3) /CreationDate (D:20240309120000Z) >>",
]
out = bytearray(b"%PDF-1.5\n")
offsets = []
for i, o in enumerate(objs, 1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R /Info 5 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
open("doc.pdf", "wb").write(out)
PY
printf 'this is not a pdf\n' > broken.pdf
printf 'hello\n' > notes.txt
ls -la
