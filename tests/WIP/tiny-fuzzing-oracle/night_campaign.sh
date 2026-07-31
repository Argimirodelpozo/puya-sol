#!/usr/bin/env bash
# Overnight generative differential campaign — FRESH seeds, all axes, sequential.
# SOLE fuzz process (concurrent compiles poison .compile_cache + _gen.sol).
# Auto-classifies divergences: the known backend DCE-drops-reverting-subexpr bug
# is ALWAYS `evm=REVERT, avm=<value>` — filtered out. INTERESTING = value-vs-value
# mismatch or `avm=REVERT` (evm value) — written to $INT for triage.
set -u
cd "$(dirname "$0")"
LOG="${1:?log path}"
INT="${2:?interesting path}"
PY=python3
GEN=fuzz_gen.py
: > "$LOG"; : > "$INT"
echo "[campaign start] $(date +%H:%M:%S)" >> "$LOG"

run_axis() {
  local name="$1" seed="$2"; shift 2
  echo "=== axis=$name seed=$seed $(date +%H:%M:%S) ===" >> "$LOG"
  rm -rf .compile_cache 2>/dev/null              # fresh cache per axis (avoid multi-run staleness)
  local tmp; tmp="$LOG.axis"
  timeout 1200 $PY $GEN --seed "$seed" "$@" > "$tmp" 2>&1
  local rc=$?
  cat "$tmp" >> "$LOG"
  echo "[exit $rc] axis=$name seed=$seed" >> "$LOG"
  # INTERESTING = divergence detail lines with avm= but NOT the known DCE (evm=REVERT) class
  grep -E "  (evm|avm)=" "$tmp" | grep "avm=" | grep -v "evm=REVERT" | while read -r ln; do
    echo "[$name seed~$seed] $ln" >> "$INT"
  done
  rm -f "$tmp"
}

base=970000
round=0
while true; do
  round=$((round+1))
  echo "########## ROUND $round base=$base $(date +%H:%M:%S) ##########" >> "$LOG"
  run_axis expr  $((base+0))    --contracts 10 --funcs 24
  run_axis cast  $((base+1000)) --contracts 10 --funcs 20 --cast
  run_axis cf    $((base+2000)) --contracts 10 --cf
  run_axis arr   $((base+3000)) --contracts 10 --arr
  run_axis bytes $((base+4000)) --contracts 10 --funcs 20 --bytes
  run_axis rich  $((base+5000)) --contracts 10 --cf --arr --cast --funcs 8   # 8 funcs → fits under 8KB cap
  base=$((base+10000))
  NI=$(grep -c . "$INT" 2>/dev/null)
  echo "[round $round done] INTERESTING-lines=$NI  $(date +%H:%M:%S)" >> "$LOG"
done
