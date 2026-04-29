#!/bin/bash
# Compile all Solady contracts systematically
# Results logged to compile_results.txt

BASEDIR="/home/argi/AlgorandFoundation/SideProjects/puya-sol/puya-sol"
PUYA_SOL="$BASEDIR/build/puya-sol"
PUYA_PATH="$BASEDIR/puya/.venv/bin/puya"
CONTRACTS="$BASEDIR/WIP/examples/solady/contracts"
WRAPPERS="$CONTRACTS/wrappers"
OUTDIR="$BASEDIR/WIP/examples/solady/out"
RESULTS="$BASEDIR/WIP/examples/solady/compile_results.txt"

echo "Solady Compilation Results - $(date)" > "$RESULTS"
echo "========================================" >> "$RESULTS"

compile() {
    local wrapper_file="$1"
    local name=$(basename "$wrapper_file" .sol)
    local out="$OUTDIR/$name"

    echo -n "Compiling $name... "

    local output
    output=$($PUYA_SOL --source "$wrapper_file" --output-dir "$out" --puya-path "$PUYA_PATH" 2>&1)
    local exit_code=$?

    if echo "$output" | grep -q "puya completed successfully"; then
        echo "OK"
        echo "PASS: $name" >> "$RESULTS"
        local warnings=$(echo "$output" | grep -c "warning:")
        echo "  Warnings: $warnings" >> "$RESULTS"
    elif [ $exit_code -ne 0 ]; then
        echo "FAIL (puya-sol error)"
        echo "FAIL: $name (puya-sol error)" >> "$RESULTS"
        echo "$output" | grep -E "error:|Error:|fatal" | head -5 >> "$RESULTS"
    else
        echo "FAIL (puya backend error)"
        echo "FAIL: $name (puya backend error)" >> "$RESULTS"
        echo "$output" | grep -E "error:|Error:" | head -5 >> "$RESULTS"
    fi
    echo "" >> "$RESULTS"
}

# Compile each wrapper
for wrapper in "$WRAPPERS"/*.sol; do
    if [ -f "$wrapper" ]; then
        compile "$wrapper"
    fi
done

echo ""
echo "========================================" >> "$RESULTS"
echo "Summary:" >> "$RESULTS"
pass=$(grep -c "^PASS:" "$RESULTS")
fail=$(grep -c "^FAIL:" "$RESULTS")
echo "  Passed: $pass" >> "$RESULTS"
echo "  Failed: $fail" >> "$RESULTS"
echo "  Total:  $((pass + fail))" >> "$RESULTS"

echo ""
echo "Results: $pass passed, $fail failed"
cat "$RESULTS"
