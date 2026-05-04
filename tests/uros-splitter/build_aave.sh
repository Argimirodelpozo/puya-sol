#!/bin/bash
# Build artifacts for the AAVE V4 HubConfigurator dance test:
#   1. HubConfigurator.sol → main + helper, splitting `authority` and
#      `setAuthority` (inherited from AccessManaged).
#   2. uros_orchestrator.py → orchestrator app.
#
# Run from this directory; outputs land at out/HubConfigurator/ and
# out/Orchestrator/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PUYA_SOL="$REPO_ROOT/build/puya-sol"
PUYA_PATH="$REPO_ROOT/puya/.venv/bin/puya"
PUYAPY="$REPO_ROOT/puya/.venv/bin/puyapy"
HERE="$(cd "$(dirname "$0")" && pwd)"

OUT="$HERE/out_aave"
rm -rf "$OUT"
mkdir -p "$OUT"

echo "=== compiling AAVE HubConfigurator with --uros-splitter ==="
ORCH_APP_ID="${UROS_ORCH_APP_ID:-0}"
"$PUYA_SOL" \
    --source "$REPO_ROOT/WIP/examples/aave-v4/contracts/HubConfigurator.sol" \
    --output-dir "$OUT/HubConfigurator" \
    --puya-path "$PUYA_PATH" \
    --uros-splitter "authority,setAuthority" \
    --uros-orch-app-id "$ORCH_APP_ID"

echo
echo "=== compiling orchestrator template ==="
cp "$REPO_ROOT/src/splitter/uros_orchestrator.py" "$OUT/orch.py"
"$PUYAPY" "$OUT/orch.py" --out-dir "$OUT/Orchestrator" --output-bytecode

echo
echo "=== sizes ==="
ls -l "$OUT/HubConfigurator/HubConfigurator.approval.bin" \
      "$OUT/HubConfigurator/__uros_split/HubConfigurator__split.approval.bin" \
      "$OUT/Orchestrator/UrosOrchestrator.approval.bin"
