#!/bin/bash
# Build everything needed for the --uros-splitter integration test:
#   1. Smoke.sol → main contract (split=`dec`) + helper at out/Smoke/...
#   2. uros_orchestrator.py → orchestrator contract at out/Orchestrator/...
#
# Run from this directory. Outputs land under tests/uros-splitter/out/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PUYA_SOL="$REPO_ROOT/build/puya-sol"
PUYA_PATH="$REPO_ROOT/puya/.venv/bin/puya"
PUYAPY="$REPO_ROOT/puya/.venv/bin/puyapy"
HERE="$(cd "$(dirname "$0")" && pwd)"

OUT="$HERE/out"
rm -rf "$OUT"
mkdir -p "$OUT"

echo "=== compiling Smoke.sol with --uros-splitter ==="
# `--uros-orch-app-id` and `--uros-storage-app-id` default to 0
# (placeholder). The end-to-end test recompiles main with the real
# app ids after orch + storage are deployed (so the bidirectional-rekey
# Sender override path can resolve __storage's address).
ORCH_APP_ID="${UROS_ORCH_APP_ID:-0}"
STORAGE_APP_ID="${UROS_STORAGE_APP_ID:-0}"
"$PUYA_SOL" \
    --source "$HERE/Smoke/Smoke.sol" \
    --output-dir "$OUT/Smoke" \
    --puya-path "$PUYA_PATH" \
    --uros-splitter "dec" \
    --uros-orch-app-id "$ORCH_APP_ID" \
    --uros-storage-app-id "$STORAGE_APP_ID"

echo
echo "=== compiling orchestrator template ==="
cp "$REPO_ROOT/src/splitter/uros_orchestrator.py" "$OUT/orch.py"
"$PUYAPY" "$OUT/orch.py" --out-dir "$OUT/Orchestrator" --output-bytecode --target-avm-version 10

echo
echo "=== compiling __storage default template ==="
cp "$REPO_ROOT/src/splitter/uros_storage.py" "$OUT/storage.py"
"$PUYAPY" "$OUT/storage.py" --out-dir "$OUT/Storage" --output-bytecode --target-avm-version 10

echo
echo "=== sizes ==="
ls -l "$OUT/Smoke/Smoke.approval.bin" \
      "$OUT/Smoke/__uros_split/chunk_0/Smoke__chunk_0.approval.bin" \
      "$OUT/Orchestrator/UrosOrchestrator.approval.bin" \
      "$OUT/Storage/UrosStorage.approval.bin"
