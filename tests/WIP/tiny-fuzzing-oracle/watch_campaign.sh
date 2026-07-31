#!/usr/bin/env bash
# Overnight watcher: polls the campaign. Returns (exits) ONLY when there is
# something for me to act on — an INTERESTING (non-DCE) finding appears, or the
# campaign process died — or after a max window so I periodically re-check.
# Prints a verdict line. Does NOT run any fuzzer itself (read-only + health).
set -u
INT="${1:?interesting path}"
CLOG="${2:?campaign log}"
MAX_ITERS="${3:-25}"      # ~25 * 60s ≈ 25 min window
i=0
while [ "$i" -lt "$MAX_ITERS" ]; do
  i=$((i+1))
  if [ -s "$INT" ]; then
    echo "VERDICT=FOUND interesting findings after ${i}m:"; head -30 "$INT"; exit 10
  fi
  if ! pgrep -f night_campaign >/dev/null 2>&1; then
    echo "VERDICT=CAMPAIGN_DEAD after ${i}m"; tail -5 "$CLOG"; exit 20
  fi
  sleep 60
done
echo "VERDICT=QUIET after ${MAX_ITERS}m — campaign healthy, no interesting findings"
grep -E "ROUND|round [0-9]+ done|diffed" "$CLOG" | tail -8
exit 0
