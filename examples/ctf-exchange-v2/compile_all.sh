#!/bin/bash
# Compile every src/ contract in ctf-exchange v1 with puya-sol.
# Logs per-file PASS/FAIL to compile_results.txt.

BASEDIR="/home/argi/AlgorandFoundation/polymarket-experiment/puya-sol"
PUYA_SOL="$BASEDIR/build/puya-sol"
PUYA_PATH="$BASEDIR/puya/.venv/bin/puya"
EXAMPLE="$BASEDIR/examples/ctf-exchange-v2"
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

    local output exit_code
    output=$("$PUYA_SOL" --source "$sol_file" "${IMPORT_PATHS[@]}" --output-dir "$out" --puya-path "$PUYA_PATH" 2>&1)
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

while IFS= read -r f; do
    compile "$f"
done < <(find "$SRC" -name "*.sol" -not -path "*/test/*" -not -name "*.s.sol" -not -name "*.t.sol" | sort)

pass=$(grep -c "^PASS:" "$RESULTS" || true)
skip=$(grep -c "^SKIP:" "$RESULTS" || true)
fail=$(grep -c "^FAIL:" "$RESULTS" || true)
echo "" >> "$RESULTS"
echo "=================================" >> "$RESULTS"
echo "Passed: $pass | Failed: $fail | Skipped (no concrete contract): $skip | Total: $((pass + fail + skip))" >> "$RESULTS"
echo ""
echo "Results: $pass passed, $fail failed, $skip skipped (see $RESULTS)"
