#!/bin/bash
# Compile all AAVE V4 deployable contracts under puya-sol → puya.
# Outputs land in WIP/examples/aave-v4/out/<Name>/, the canonical
# location the existing pytest suite reads from.
#
# Excludes the EVM proxy stack (ERC1967Proxy, TransparentUpgradeableProxy,
# ProxyAdmin) — see patches.md.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PUYA_SOL="$REPO_ROOT/build/puya-sol"
PUYA_PATH="$REPO_ROOT/puya/.venv/bin/puya"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"

# Deployable contracts that should compile and end up in `out/` for tests.
# Includes the 12 puya-sol-clean contracts plus the smaller wrappers.
CONTRACTS=(
    AaveOracle
    AccessManager
    AccessManagerEnumerable
    AssetInterestRateStrategy
    ConfigPermissionsMapWrapper
    ConfigPositionManager
    EIP712HashWrapper
    GiverPositionManager
    Hub
    HubConfigurator
    IntentConsumer
    MathUtilsWrapper
    NoncesKeyed
    PercentageMathWrapper
    PositionStatusMapWrapper
    PremiumWrapper
    ReserveFlagsMapWrapper
    RolesWrapper
    SharesMathWrapper
    SignatureGateway
    Spoke
    SpokeConfigurator
    SpokeInstance
    SpokeUtilsWrapper
    TakerPositionManager
    TokenizationSpoke
    TokenizationSpokeInstance
    TreasurySpoke
    UnitPriceFeed
    WadRayMathWrapper
    WETH9
)

pass=0
fail=0
fails=()

for c in "${CONTRACTS[@]}"; do
    src="$HERE/contracts/$c.sol"
    if [ ! -f "$src" ]; then
        echo "SKIP  $c  (no source)"
        continue
    fi
    rm -rf "$OUT/$c"
    out=$("$PUYA_SOL" \
        --source "$src" \
        --output-dir "$OUT/$c" \
        --puya-path "$PUYA_PATH" 2>&1)
    if echo "$out" | grep -q "puya completed successfully"; then
        if [ -f "$OUT/$c/$c.approval.bin" ]; then
            sz=$(wc -c < "$OUT/$c/$c.approval.bin")
            printf "PASS  %-30s %6d B\n" "$c" "$sz"
        else
            printf "PASS  %-30s (abstract — no main bin)\n" "$c"
        fi
        pass=$((pass+1))
    else
        err=$(echo "$out" | grep -E "error:|critical:" | head -1 | sed 's/.*error: //; s/.*critical: //' | cut -c1-90)
        printf "FAIL  %-30s %s\n" "$c" "$err"
        fail=$((fail+1))
        fails+=("$c")
    fi
done

echo
echo "================================================"
echo "  passed: $pass    failed: $fail"
if [ ${#fails[@]} -gt 0 ]; then
    echo "  failing: ${fails[*]}"
fi
