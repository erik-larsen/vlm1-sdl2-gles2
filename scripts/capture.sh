#!/bin/sh
# Deterministic frame capture for fidelity/regression work.
# Builds the GL-free capture harness (rebuilding when sources change),
# renders one preset, and writes a PNG.
#
#   scripts/capture.sh <bank 0-9> <effect 1-9> [frames] [out.png]
#
# bank is 0-indexed (0 = "Bank 1"); frames defaults to 180 (enough for
# DVF feedback to establish). Pass -o to open the PNG when done.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

open_it=0
[ "${1:-}" = "-o" ] && { open_it=1; shift; }

if [ $# -lt 2 ]; then
    echo "usage: scripts/capture.sh [-o] <bank 0-9> <effect 1-9> [frames] [out.png]" >&2
    exit 2
fi
bank=$1 effect=$2
frames=${3:-180}
out=${4:-bin-test/cap_b${bank}e${effect}.png}

bin=bin-test/capture
mkdir -p bin-test

# Rebuild if the binary is missing or any source is newer than it.
need_build=0
[ -x "$bin" ] || need_build=1
if [ "$need_build" = 0 ]; then
    if find src tests/capture.c -name '*.c' -newer "$bin" 2>/dev/null | grep -q .; then
        need_build=1
    fi
fi
if [ "$need_build" = 1 ]; then
    echo "building $bin ..." >&2
    # everything except the SDL/GL/app layer
    srcs=$(ls src/*.c | grep -vE 'main|platform|editor|board|audiofft|glfuncs')
    # shellcheck disable=SC2086
    cc -std=gnu17 -O2 -Isrc tests/capture.c $srcs -lm -o "$bin"
fi

ppm=$(mktemp -t vlmcap).ppm
"$bin" "$bank" "$effect" "$frames" "$ppm"
python3 -c "from PIL import Image; Image.open('$ppm').save('$out')"
rm -f "$ppm"
echo "$out"
[ "$open_it" = 1 ] && command -v open >/dev/null && open "$out" || true
