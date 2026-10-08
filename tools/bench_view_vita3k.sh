#!/bin/sh
# Frame cost of the start of C01A without input (the bedroom window view, the
# heaviest indoor view early in the game) in Vita3K at the console's CPU clock,
# with the benchmark clock on (bench_clock= in ff.cfg: game time advances 1/60 s
# per frame, so every run plays the same frames). Prints the real ms per frame
# of each 300-frame range and the average over frames 600..2400, with the time
# the game thread was busy (not waiting for the render thread or the vsync): the
# number to compare builds by.
# usage: tools/bench_view_vita3k.sh [build dir]   (default build-vita)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build-vita}
CFG="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/ff/ff.cfg"
SAVED=
[ -f "$CFG" ] && SAVED=$(cat "$CFG")
echo "bench_clock=16667" > "$CFG"
BUILD=$BUILD TIMEOUT=600 "$ROOT/tools/qa_vita3k.sh" "41000:quit" "$BUILD/bench" > /dev/null 2>&1 || true
if [ -n "$SAVED" ]; then printf '%s\n' "$SAVED" > "$CFG"; else rm -f "$CFG"; fi
grep "bench: frames" "$BUILD/bench/ff.log" | sed 's/.*bench: //'
awk '/bench: frames/ { split($5, r, "[-:]"); if (r[1] >= 600 && r[2] <= 2400) { n++; s += $6; b += $13 } }
     END { if (n) printf "average frames 600-2400: %.2f ms per frame (%.1f fps), game thread busy %.2f ms\n", s/n, 1000/(s/n), b/n; else print "no data" }' \
    "$BUILD/bench/ff.log"
