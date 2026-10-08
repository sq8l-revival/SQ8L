#!/bin/sh
# Drive the Linux editor in a virtual X server (Xvfb + xdotool): the VST2 plugin runs in
# tests/uihost/linux_host.cpp, every step is followed by a screenshot <out>/<n>.png.
#   tests/linux_ui_session.sh build/bin/sq8l-vst2.so out "c 105,12; m 120,40; k Escape; t abc"
#   c x,y = left click, r x,y = right click, m x,y = move, k Key = xdotool key, t text = typing
set -e
PLUGIN=$1; OUT=$2; STEPS=$3
mkdir -p "$OUT" /tmp/sq8l-home
rm -f /tmp/sq8l-stop
Xvfb :99 -screen 0 800x600x24 +extension GLX >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:99
# wait until the server answers (a fixed delay is sometimes too short on CI runners)
i=0
until xdotool getdisplaygeometry >/dev/null 2>&1; do
    i=$((i + 1))
    [ $i -gt 100 ] && { echo "Xvfb did not start"; exit 1; }
    sleep 0.1
done
g++ -O1 -o /tmp/linux_host "$(dirname "$0")/uihost/linux_host.cpp" -lX11 -ldl
HOME=/tmp/sq8l-home SQ8L_UI_DEBUG=1 timeout -s KILL 150 /tmp/linux_host "$PLUGIN" 140 /tmp/sq8l-stop > "$OUT/host.log" 2>&1 &
HOST=$!
sleep 3
import -window root -crop 626x430+0+0 "$OUT/0.png"
n=0
echo "$STEPS" | tr ';' '\n' | while read -r cmd arg; do
    [ -z "$cmd" ] && continue
    n=$((n + 1))
    x=${arg%,*}; y=${arg#*,}
    case $cmd in
        c) xdotool mousemove "$x" "$y" click 1 ;;
        r) xdotool mousemove "$x" "$y" click 3 ;;
        m) xdotool mousemove "$x" "$y" ;;
        k) xdotool key "$arg" ;;
        t) xdotool type --delay 60 "$arg" ;;
    esac
    sleep 0.7
    if [ -n "$FULL" ]; then import -window root "$OUT/$n.png"; else import -window root -crop 626x430+0+0 "$OUT/$n.png"; fi
    echo "step $n: $cmd $arg"
done
touch /tmp/sq8l-stop
wait $HOST || true
kill $XVFB 2>/dev/null || true
echo "host: $(tail -1 "$OUT/host.log")"
