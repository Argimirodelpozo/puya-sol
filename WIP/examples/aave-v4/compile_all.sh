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
    NoncesKeyedMock
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

# Per-contract --uros-splitter group lists. Each entry is one chunk's
# method names. Required for contracts whose unsplit approval is over
# the AVM 4-page (8 KB) deploy limit. Contracts not listed here compile
# normally without splitting.
declare -A SPLIT_GROUPS
SPLIT_GROUPS[AccessManagerEnumerable]=$'getRole,getRoleCount,getRoles,getRoleMember,getRoleMemberCount,getRoleMembers,getRoleTarget,getRoleTargetCount,getRoleTargets|getAdminRole,getAdminRoleCount,getAdminRoles,getRoleOfAdminRole,getRoleOfAdminRoleCount,getRolesOfAdminRole,getRoleTargetSelector,getRoleTargetSelectorCount,getRoleTargetSelectors|expiration,minSetback,isTargetClosed,getTargetFunctionRole,getTargetAdminDelay,getRoleAdmin,getRoleGuardian,getRoleGrantDelay,getAccess,hasRole'

for c in "${CONTRACTS[@]}"; do
    src="$HERE/contracts/$c.sol"
    if [ ! -f "$src" ]; then
        echo "SKIP  $c  (no source)"
        continue
    fi
    rm -rf "$OUT/$c"
    args=(--source "$src" --output-dir "$OUT/$c" --puya-path "$PUYA_PATH")
    if [ -n "${SPLIT_GROUPS[$c]:-}" ]; then
        IFS='|' read -ra groups <<< "${SPLIT_GROUPS[$c]}"
        for g in "${groups[@]}"; do
            args+=(--uros-splitter "$g")
        done
    fi
    out=$("$PUYA_SOL" "${args[@]}" 2>&1)
    if echo "$out" | grep -q "puya completed successfully"; then
        if [ -f "$OUT/$c/$c.approval.bin" ]; then
            sz=$(wc -c < "$OUT/$c/$c.approval.bin")
            tag=""
            [ -n "${SPLIT_GROUPS[$c]:-}" ] && tag=" [split]"
            printf "PASS  %-30s %6d B%s\n" "$c" "$sz" "$tag"
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
