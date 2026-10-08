#!/bin/sh
# Automated visual check of the Vita build in Vita3K: boots straight into a level
# (a test copy of the game directory whose Freedom.ini has DefaultScene=<scene>),
# plays a scripted input (ux0:data/ff/input.txt, the FF_INPUT syntax of
# shim/src/platform_common.c), captures the Vita3K window at every "shot=NAME" of
# the script into $OUT/NAME.png and stops at "quit" (or the timeout).
#
# usage: tools/qa_vita3k.sh "<script>" [out dir]
#   e.g. tools/qa_vita3k.sh "3000:mx+2000,4000:my+400,6000:shot=floor,7000:quit"
# env:   BUILD   Vita build directory (default build-vita)
#        SCENE   start scene (default SinglePlayer\C01A\FF-C01A_MAIN.gms)
#        MHZ     emulated CPU clock (default 444; the timing then resembles hardware's)
#        VITA3K  Vita3K binary (default the local source build)
#        TIMEOUT seconds (default 300)
# The installed eboot and ux0:data/ff/pc are put back afterwards; only the Vita3K
# instance started here ("Vita3K -r FFRD00001") is stopped.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=${1:?usage: tools/qa_vita3k.sh "<script>" [out dir]}
OUT=${2:-$ROOT/build-vita/qa}
BUILD=${BUILD:-$ROOT/build-vita}
SCENE=${SCENE:-'SinglePlayer\C01A\FF-C01A_MAIN.gms'}
VITA3K=${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}
TIMEOUT=${TIMEOUT:-300}
UX0="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0"
APP="$UX0/app/FFRD00001"
DATA="$UX0/data/ff"
QA="$ROOT/gamedata/qa"
LOG=${TMPDIR:-/tmp}/ff_qa_vita3k.log

[ -f "$BUILD/ff.vpk" ] || { echo "no $BUILD/ff.vpk; build first" >&2; exit 1; }
[ -x "$VITA3K" ] || { echo "no Vita3K at $VITA3K" >&2; exit 1; }
[ -d "$APP" ] || { echo "FFRD00001 is not installed in Vita3K (tools/install_vita3k.sh)" >&2; exit 1; }
if pgrep -f "Vita3K -r FFRD00001" > /dev/null; then
    echo "a Vita3K instance running FFRD00001 already exists; close it first" >&2
    exit 1
fi

# The test copy of the game directory: links to gamedata/pc, its own Freedom.ini.
rm -rf "$QA"
mkdir -p "$QA"
for f in "$ROOT"/gamedata/pc/*; do
    n=$(basename "$f")
    [ "$n" = "Freedom.ini" ] || ln -s "../pc/$n" "$QA/$n"
done
# (backslashes doubled: sed would read \C, \F... as escapes)
SCENE_SED=$(printf '%s' "$SCENE" | sed 's/\\/\\\\/g')
sed "s|^DefaultScene=.*|DefaultScene=$SCENE_SED|" "$ROOT/gamedata/pc/Freedom.ini" > "$QA/Freedom.ini"

SAVED_EBOOT=${TMPDIR:-/tmp}/ff_qa_saved_eboot.$$
cp "$APP/eboot.bin" "$SAVED_EBOOT"
unzip -p "$BUILD/ff.vpk" eboot.bin > "$APP/eboot.bin" || exit 1
SAVED_PC=$(readlink "$DATA/pc")
ln -sfn "$QA" "$DATA/pc"
printf '%s' "$SCRIPT" > "$DATA/input.txt"
rm -f "$DATA/shot_now.txt" "$DATA/ff.log" "$DATA/profile.txt"
mkdir -p "$OUT"
touch "$OUT"

stop() {
    pkill -9 -f "Vita3K -r FFRD00001" 2>/dev/null
    rm -f "$DATA/input.txt" "$DATA/shot_now.txt"
    ln -sfn "$SAVED_PC" "$DATA/pc"
    mv -f "$SAVED_EBOOT" "$APP/eboot.bin"
}
trap 'stop; exit 1' INT TERM

WINID_BIN=${TMPDIR:-/tmp}/ff_window_id
[ "$WINID_BIN" -nt "$ROOT/tools/window_id.swift" ] || swiftc -O "$ROOT/tools/window_id.swift" -o "$WINID_BIN" || exit 1

(VITA3K_CPU_MHZ=${MHZ:-444} "$VITA3K" -r FFRD00001 > "$LOG" 2>&1 &)
started=0
while [ $started -lt 30 ] && ! pgrep -f "Vita3K -r FFRD00001" > /dev/null; do
    sleep 1
    started=$((started + 1))
done
shots=0
waited=0
while [ $waited -lt $((TIMEOUT * 2)) ] && pgrep -f "Vita3K -r FFRD00001" > /dev/null; do
    if [ -s "$DATA/shot_now.txt" ]; then
        name=$(cat "$DATA/shot_now.txt")
        rm -f "$DATA/shot_now.txt"
        pid=$(pgrep -f "Vita3K -r FFRD00001" | head -n 1)
        win=$("$WINID_BIN" "$pid" "(FFRD00001)" | head -n 1)
        if [ -n "$win" ] && screencapture -x -o -l "$win" "$OUT/$name.png"; then
            shots=$((shots + 1))
            echo "shot $name"
        else
            echo "shot $name: no Vita3K window" >&2
        fi
    fi
    # The game exits at "quit" but Vita3K stays open: its log says so.
    if grep -aq "\[input\] quit" "$DATA/ff.log" 2>/dev/null; then
        break
    fi
    sleep 0.5
    waited=$((waited + 1))
done
stop
if [ -s "$DATA/profile.txt" ] && [ "$DATA/profile.txt" -nt "$OUT" ]; then
    cp "$DATA/profile.txt" "$OUT/profile.txt"
    echo "profile: $OUT/profile.txt"
fi
cp "$DATA/ff.log" "$OUT/ff.log" 2>/dev/null
echo "$shots screenshot(s) in $OUT; log: $OUT/ff.log"
