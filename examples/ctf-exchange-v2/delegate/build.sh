#!/usr/bin/env bash
# Build the lonely-chunk approval program for matchOrders.
#
# Steps:
#  1. Compile F-as-approval. F is a standalone program that operates on
#     the orchestrator's storage layout. For now we use the un-split
#     orchestrator's approval as F (it's >8192 so this won't directly
#     deploy, but the install path via UpdateApplication CAN target up
#     to 8192 — we'll need to slim F before this works end-to-end).
#  2. Stitch F's bytes into the lonely-chunk template.
#  3. Compile the resulting TEAL via algod's /v2/teal/compile endpoint.
#
# Until F-as-approval emission is properly wired into the splitter, this
# script is a placeholder demonstrating the assembly steps.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
V2=$(cd "$HERE/.." && pwd)
PUYA_SOL="$V2/../../build/puya-sol"
PUYA_PATH="$V2/../../puya/.venv/bin/puya"

# Step 1: build F-as-approval. PLACEHOLDER — uses helper3's awst.json
# (which currently fails to compile because of unresolved
# InstanceMethodTarget refs to orchestrator-internal methods like
# `_matchOrders`). The proper fix is to extend SimpleSplitter to pull in
# all transitive method deps for delegate functions.
F_DIR="$V2/out/exchange/CTFExchange/CTFExchange__Helper3"
echo "F directory: $F_DIR"
if [ ! -f "$F_DIR/CTFExchange__Helper3.approval.bin" ]; then
    echo "F is not yet compiled (helper3 awst.json has unresolved refs)."
    echo "Skipping stitch."
    exit 0
fi

# Step 2: stitch
ORCH_CLEAR="$V2/out/exchange/CTFExchange/CTFExchange/CTFExchange.clear.bin"
python3 "$HERE/stitch.py" \
    "$F_DIR/CTFExchange__Helper3.approval.bin" \
    "$ORCH_CLEAR" \
    "$HERE/lonely_chunk.teal"

# Step 3: TODO compile via algod
echo "Stitched TEAL at $HERE/lonely_chunk.teal"
echo "Next: compile via algod and deploy as the matchOrders sidecar."
