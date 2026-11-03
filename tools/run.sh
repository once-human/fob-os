#!/usr/bin/env bash
# usage: bash run.sh <script-name-without-.py> [args...]
#   e.g. bash run.sh probe
#        bash run.sh upload_face faces/hello.bin
cd "$(dirname "$0")" || exit 1
name="${1:-probe}"; shift
ts=$(date +%Y%m%d-%H%M%S)
out="logs/${ts}-${name}.txt"
echo "== $name $* @ $ts" | tee "$out"
"$HOME/watchenv/bin/python" -u "${name}.py" "$@" 2>&1 | tee -a "$out"
echo "== finished. log: $PWD/$out" | tee -a "$out"
# one low warning beep = finished
paplay /usr/share/sounds/freedesktop/stereo/dialog-warning.oga 2>/dev/null &
