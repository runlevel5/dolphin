#!/bin/bash
# Runs a game at normal speed with the Null video backend and dual core, and prints how busy the CPU
# thread was. With perf, records a profile of the CPU thread, with JIT blocks named by a perf map.
# Usage: bench.sh <dolphin-emu-nogui> <game> <core number> <seconds> [perf.data]
set -u
dolphin=$1
game=$2
core=$3
seconds=$4
perf_data=${5:-}

user=$(mktemp -d)
map=$(mktemp -d)
trap 'rm -rf "$user" "$map"' EXIT

"$dolphin" -u "$user" -p headless -v Null -e "$game" \
  -C Dolphin.Core.CPUCore="$core" \
  -C Dolphin.Core.CPUThread=True \
  -C Dolphin.DSP.Backend="No Audio Output" \
  -C Dolphin.Core.PerfMapDir="$map" > /dev/null 2>&1 &
pid=$!

# Skip the boot.
sleep 10
cpu_thread=$(grep -l "^CPU thread$" /proc/$pid/task/*/comm | head -1 | cut -d/ -f5)
ticks() { awk '{ print $14 + $15 }' /proc/$pid/task/$cpu_thread/stat; }
start=$(ticks)
if [ -n "$perf_data" ]; then
  perf record -q -t "$cpu_thread" -o "$perf_data" -- sleep "$seconds" 2> /dev/null
else
  sleep "$seconds"
fi
end=$(ticks)
hz=$(getconf CLK_TCK)
cp "$map"/perf-*.map /tmp/ 2> /dev/null
kill $pid
wait $pid 2> /dev/null
echo "core $core: CPU thread busy $(( (end - start) * 100 / (hz * seconds) ))% of $seconds s"
