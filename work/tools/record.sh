#!/usr/bin/env bash
# Record the game window (video + desktop audio) to an MP4.
#   work/tools/record.sh [output.mp4]
# Start it before or after the game (work/run.sh); it
# waits for the window. Press q (or Ctrl-C) to stop; the file is finalized then.
# Default output: recordings/<timestamp>.mp4 in the repo root.
#
# The game uses SDL2, which runs under XWayland on a Wayland desktop, so
# x11grab can capture its window. Encoding is NVENC H.264 when available,
# otherwise libx264.
#   FPS=60          capture rate
#   QUALITY=20      constant quality (lower is better and bigger)
#   AUDIO=0         no audio
#   AUDIO_SRC=...   PulseAudio/PipeWire source (default: monitor of the default sink)
#   TITLE=...       window title to look for (default "MotorStorm")
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FPS="${FPS:-60}"
QUALITY="${QUALITY:-20}"
TITLE="${TITLE:-MotorStorm}"
FFMPEG="${FFMPEG:-/usr/bin/ffmpeg}"
[ -x "$FFMPEG" ] || FFMPEG=ffmpeg
out="${1:-$ROOT/recordings/$(date +%Y%m%d-%H%M%S).mp4}"
mkdir -p "$(dirname "$out")"

echo "record: waiting for a window titled '$TITLE'..."
wid=""
while [ -z "$wid" ]; do
    wid="$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | head -1 || true)"
    [ -n "$wid" ] || sleep 0.5
done
sleep 1   # let the window settle at its final size (maximize/fullscreen)
geom="$(xwininfo -id "$wid" | awk '/Width:/{w=$2} /Height:/{h=$2} END{print w"x"h}')"
echo "record: window $wid ($geom) -> $out"

encoders="$("$FFMPEG" -hide_banner -encoders 2>/dev/null || true)"
if [[ "$encoders" == *h264_nvenc* ]]; then
    venc=(-c:v h264_nvenc -preset p5 -rc vbr -cq "$QUALITY" -b:v 0 -pix_fmt yuv420p)
else
    venc=(-c:v libx264 -preset veryfast -crf "$QUALITY" -pix_fmt yuv420p)
fi

ain=(); aenc=()
if [ "${AUDIO:-1}" = 1 ]; then
    src="${AUDIO_SRC:-$(pactl get-default-sink 2>/dev/null).monitor}"
    ain=(-thread_queue_size 1024 -f pulse -i "$src")
    aenc=(-c:a aac -b:a 192k)
fi

exec "$FFMPEG" -hide_banner -loglevel warning -stats \
    -thread_queue_size 1024 -f x11grab -framerate "$FPS" -draw_mouse 0 \
    -window_id "$wid" -i "${DISPLAY:-:0}" \
    "${ain[@]}" \
    -vf "crop=trunc(iw/2)*2:trunc(ih/2)*2" \
    "${venc[@]}" "${aenc[@]}" \
    -movflags +faststart "$out"
