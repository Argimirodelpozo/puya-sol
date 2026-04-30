#!/bin/bash
# Compile every src/ contract in ctf-exchange v1 with puya-sol.
# Logs per-file PASS/FAIL to compile_results.txt.

BASEDIR="/home/argi/AlgorandFoundation/polymarket-experiment/puya-sol"
PUYA_SOL="$BASEDIR/build/puya-sol"
PUYA_PATH="$BASEDIR/puya/.venv/bin/puya"
EXAMPLE="$BASEDIR/WIP/examples/polymarket/ctf-exchange-v2"
SRC="$EXAMPLE/src"
OUTDIR="$EXAMPLE/out"
RESULTS="$EXAMPLE/compile_results.txt"

mkdir -p "$OUTDIR"
echo "ctf-exchange v2 compile - $(date)" > "$RESULTS"
echo "=================================" >> "$RESULTS"

# In-tree subdirs (common/, dev/, exchange/) resolve via --import-path src
# External libs (openzeppelin/solady/solmate/forge-std) resolve via deps/ symlinks
IMPORT_PATHS=(
    --import-path "$EXAMPLE/deps"
    --import-path "$EXAMPLE/lib"
)

compile() {
    local sol_file="$1"
    local rel="${sol_file#$SRC/}"
    local out="$OUTDIR/${rel%.sol}"
    mkdir -p "$out"

    echo -n "[$rel] "

    # CTFExchange.sol gets matchOrders force-delegated into a lonely-chunk
    # sidecar — without this the orch is 10.7KB (over 8KB cap). split.json
    # already moves auxiliary helpers to Helper1/Helper2; the delegate is
    # what brings the orch under the cap.
    local extra_args=()
    if [[ "$rel" == "exchange/CTFExchange.sol" ]]; then
        extra_args+=(--split-config "$EXAMPLE/split.json" --force-delegate matchOrders)
        # matchOrders + ECDSA recovers + getCollectionId/getPositionId all
        # blow past the 700-op single-tx budget. Pump via ensure_budget so
        # the runtime opup pool covers two ECDSA recovers (~1700 ea) + body.
        extra_args+=(--ensure-budget matchOrders:100000)
        extra_args+=(--ensure-budget CTHelpers.getCollectionId:30000)
        extra_args+=(--ensure-budget CTHelpers.getPositionId:30000)
    elif [[ "$rel" == "adapters/CtfCollateralAdapter.sol" ]]; then
        # CtfCollateralAdapter.{splitPosition,mergePositions,redeemPositions}
        # chain through CT.transferFrom + CT.unwrap + CTHelpers (getCollectionId,
        # getPositionId — keccak-heavy) + CTF.splitPosition/etc. The inner-call
        # cascade blows past the 700-op single-tx budget.
        extra_args+=(--ensure-budget splitPosition:80000)
        extra_args+=(--ensure-budget mergePositions:80000)
        extra_args+=(--ensure-budget redeemPositions:80000)
    elif [[ "$rel" == "adapters/NegRiskCtfCollateralAdapter.sol" ]]; then
        extra_args+=(--ensure-budget splitPosition:80000)
        extra_args+=(--ensure-budget mergePositions:80000)
        extra_args+=(--ensure-budget redeemPositions:80000)
        extra_args+=(--ensure-budget convertPositions:80000)
    elif [[ "$rel" == "collateral/PermissionedRamp.sol" ]]; then
        # PermissionedRamp.__postInit runs the inherited Solady EIP712
        # constructor (computeDomainSeparator: keccak256 of the typed-data
        # name/version + chainID + verifyingContract + salt). That's ~3 K
        # opcodes of bytes-mashing + a sha512_256, well past the 700-op
        # per-tx budget. Pump via ensure_budget; matches the matchOrders
        # pattern. wrap/unwrap also do EIP-712 typed-data hashing +
        # ECDSA recover + role check + 2 inner txns; pump those too.
        extra_args+=(--ensure-budget __postInit:100000)
        extra_args+=(--ensure-budget wrap:50000)
        extra_args+=(--ensure-budget unwrap:50000)
    fi

    local output exit_code
    output=$("$PUYA_SOL" --source "$sol_file" "${IMPORT_PATHS[@]}" --output-dir "$out" --puya-path "$PUYA_PATH" "${extra_args[@]}" 2>&1)
    exit_code=$?

    if echo "$output" | grep -q "puya completed successfully"; then
        echo "OK"
        echo "PASS: $rel" >> "$RESULTS"
    elif echo "$output" | grep -q "No contracts found"; then
        echo "SKIP (no concrete contract)"
        echo "SKIP: $rel (no concrete contract — interface/library/abstract)" >> "$RESULTS"
    elif [ $exit_code -ne 0 ]; then
        echo "FAIL"
        echo "FAIL: $rel" >> "$RESULTS"
        echo "$output" | grep -E "error:|critical:|fatal" | head -5 | sed 's/^/    /' >> "$RESULTS"
    else
        echo "FAIL (puya backend)"
        echo "FAIL: $rel (puya backend error)" >> "$RESULTS"
        echo "$output" | grep -E "error:|critical:|Error:" | head -5 | sed 's/^/    /' >> "$RESULTS"
    fi
}

# Compile production sources plus the test-only mocks under
# src/test/dev/mocks/ — the latter are needed by the python test fixtures
# (USDC, ERC1271 mocks, UniversalMock, etc.). Other test/ paths (Foundry
# .t.sol/.s.sol files, BaseExchangeTest helpers) are skipped: they're
# Foundry-specific and don't survive puya-sol translation.
while IFS= read -r f; do
    compile "$f"
done < <(find "$SRC" -name "*.sol" \
    -not -name "*.s.sol" -not -name "*.t.sol" \
    \( -not -path "*/test/*" -o -path "*/test/dev/mocks/*" \) | sort)

pass=$(grep -c "^PASS:" "$RESULTS" || true)
skip=$(grep -c "^SKIP:" "$RESULTS" || true)
fail=$(grep -c "^FAIL:" "$RESULTS" || true)
echo "" >> "$RESULTS"
echo "=================================" >> "$RESULTS"
echo "Passed: $pass | Failed: $fail | Skipped (no concrete contract): $skip | Total: $((pass + fail + skip))" >> "$RESULTS"
echo ""
echo "Results: $pass passed, $fail failed, $skip skipped (see $RESULTS)"
