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
SPLIT_GROUPS[AccessManagerEnumerable]=$'getRole,getRoleCount,getRoles,getRoleMember,getRoleMemberCount,getRoleMembers,getRoleTarget,getRoleTargetCount,getRoleTargets|getAdminRole,getAdminRoleCount,getAdminRoles,getRoleOfAdminRole,getRoleOfAdminRoleCount,getRolesOfAdminRole,getRoleTargetSelector,getRoleTargetSelectorCount,getRoleTargetSelectors|expiration,minSetback,isTargetClosed,getTargetFunctionRole,getTargetAdminDelay,getRoleAdmin,getRoleGuardian,getRoleGrantDelay,getAccess,hasRole|labelRole,grantRole,revokeRole,renounceRole,setRoleAdmin,setRoleGuardian,setGrantDelay,setTargetFunctionRole,setTargetAdminDelay,setTargetClosed,getSchedule,getNonce,updateAuthority,ADMIN_ROLE,PUBLIC_ROLE,canCall'

# Hub.sol — 20 KB unsplit, 65 methods. 11 chunks all fit under the
# 4-page (8 KB) AVM deploy limit. Bin-packed by domain:
#   3 view chunks (asset getters all-in-one, spoke getters, previews)
#   2 paired mutators (add+remove, draw+restore)
#   2 small-mutator clusters (transferShares+refreshPremium+mintFeeShares
#     +payFeeShares; sweep+reclaim+reportDeficit)
#   3 isolated heavy mutators (eliminateDeficit, addAsset, the
#     updateAssetConfig+addSpoke pair)
#   1 spoke-config cluster (updateSpokeConfig+setInterestRateData)
SPLIT_GROUPS[Hub]=$'getAssetUnderlyingAndDecimals,getAssetDrawnIndex,getAddedAssets,getAddedShares,getAssetOwed,getAssetTotalOwed,getAssetPremiumRay,getAssetDrawnShares,getAssetPremiumData,getAssetLiquidity,getAssetDeficitRay,getAsset,getAssetConfig,getAssetAccruedFees,getAssetSwept,getAssetDrawnRate|getSpokeCount,getSpokeAddedAssets,getSpokeAddedShares,getSpokeOwed,getSpokeTotalOwed,getSpokePremiumRay,getSpokeDrawnShares,getSpokePremiumData,getSpokeDeficitRay,getSpokeAddress,getSpoke,getSpokeConfig|getAssetId,getAssetCount,previewAddByAssets,previewAddByShares,previewRemoveByAssets,previewRemoveByShares,previewDrawByAssets,previewDrawByShares,previewRestoreByAssets,previewRestoreByShares,isUnderlyingListed,isSpokeListed|add,remove|draw,restore|transferShares,refreshPremium,mintFeeShares,payFeeShares|sweep,reclaim,reportDeficit|eliminateDeficit|addAsset|updateAssetConfig,addSpoke|updateSpokeConfig,setInterestRateData'

# SpokeInstance.sol — 23 KB unsplit, 58 methods (inherited from Spoke).
# Bin-pack: 8 chunks by domain. Methods kept on main (not in any
# chunk): __postInit, initialize, eip712Domain, multicall, extSload,
# extSloads. eip712Domain/multicall/extSloads return variable-length
# types whose forwarding-stub decode would break tuple shape; extSload
# calls __storage_read(slot) with bytes[32] but the chunk's
# __storage_read stub has uint64 arg signature → typecheck fails. Both
# stay on main where the original body runs directly.
SPLIT_GROUPS[SpokeInstance]=$'__postInit,SPOKE_REVISION,SET_USER_POSITION_MANAGERS_TYPEHASH,MAX_USER_RESERVES_LIMIT,ORACLE,DOMAIN_SEPARATOR,getLiquidationConfig,getLiquidationLogic,getLiquidationBonus|getReserveCount,getReserveSuppliedAssets,getReserveSuppliedShares,getReserveDebt,getReserveTotalDebt,getReserveId,getReserve,getReserveConfig,getDynamicReserveConfig|getUserReserveStatus,getUserSuppliedAssets,getUserSuppliedShares,getUserDebt,getUserTotalDebt,getUserPremiumDebtRay,getUserPosition,getUserLastRiskPremium,getUserAccountData,isPositionManagerActive,isPositionManager|updateLiquidationConfig,addReserve,updateReserveConfig,updateReservePriceSource,addDynamicReserveConfig,updateDynamicReserveConfig,updatePositionManager|borrow|repay|liquidationCall|supply|withdraw|setUsingAsCollateral|updateUserRiskPremium|updateUserDynamicConfig|setUserPositionManager|setUserPositionManagersWithSig|renouncePositionManagerRole|permitReserve|useNonce,nonces|authority,setAuthority,isConsumingScheduledOp,initialize'

# Per-contract opt-in to --deploy-pure-helpers. SpokeInstance's
# liquidationCall chunk needs ~700 B trimmed to fit under the AVM
# 8 KB page cap; lifting the heavy LiquidationLogic pure helpers into
# sidecar apps does it.
declare -A DEPLOY_PURE_HELPERS
DEPLOY_PURE_HELPERS[SpokeInstance]=1

# Per-contract --fn-split specs. Space-separated list, one --fn-split
# arg per token. Format: <Method>:<idx>,<idx>,...:g<N>[:cross]
# - Without :cross — pieces share the same txn frame (load 100 prologue);
#   pair with --uros-splitter to keep them in the SAME chunk.
# - With :cross — pieces live on SEPARATE chunks and chain via
#   orch.dispatch_chain (gload prologue). main.cpp emits chain_groups.json
#   which the dance harness reads to register each chain with orch.
#
# SpokeInstance.liquidationCall (10878 B standalone, > 8 KB AVM cap)
# splits into 2 cross-chunk pieces of ~5400 B each — fits the
# dispatch_chain v2 4-page-per-piece cap. Split index 3 is the natural
# midpoint of the 6-stmt body (3 ABI-decode ExpressionStatements
# followed by 2 AssignmentStatements + a return).
declare -A FN_SPLITS
# SpokeInstance.liquidationCall is 10878 B standalone — needs splitting
# but a 2-piece :cross split doesn't help: both pieces transitively call
# `_calculateUserAccountData` (one in main body, one in else branch),
# which pulls in `_processUserAccountData` (~7 KB internal). Either
# branch's chunk ends up bigger than the AVM 8 KB cap.
# Resolution path requires task #63 (dispatch_chain v3, chain_len ≥ 3)
# AND a non-pure sidecar pattern for `_processUserAccountData` (state
# reads via inner-call back to __storage), OR a source-level refactor
# that reuses the first call's UserAccountData in the else branch.
# Until then, liquidationCall stays as a single chunk and trips the
# runtime size assertion in deploy_split_app — the 3 oracle tests that
# need SpokeInstance deploy skip cleanly with a clear reason.
# FN_SPLITS[SpokeInstance]="LiquidationLogic._executeLiquidation:4:g0:cross"
# Tried: pieces stay in the same chunk because SPLIT_GROUPS only routes
# ABI methods, not internal-sub pieces. chunk_6 grew slightly. Needs
# either internal-sub-pieces-as-sidecars or ABI-promotion to help.

for c in "${CONTRACTS[@]}"; do
    src="$HERE/contracts/$c.sol"
    if [ ! -f "$src" ]; then
        echo "SKIP  $c  (no source)"
        continue
    fi
    rm -rf "$OUT/$c"
    args=(--source "$src" --output-dir "$OUT/$c" --puya-path "$PUYA_PATH")
    if [ -n "${DEPLOY_PURE_HELPERS[$c]:-}" ]; then
        args+=(--deploy-pure-helpers)
    fi
    # --fn-split must precede --uros-splitter so the emitted piece
    # methods exist in AWST when the chunk groups reference them by name.
    if [ -n "${FN_SPLITS[$c]:-}" ]; then
        IFS=' ' read -ra splits <<< "${FN_SPLITS[$c]}"
        for s in "${splits[@]}"; do
            args+=(--fn-split "$s")
        done
    fi
    if [ -n "${SPLIT_GROUPS[$c]:-}" ]; then
        IFS='|' read -ra groups <<< "${SPLIT_GROUPS[$c]}"
        for g in "${groups[@]}"; do
            args+=(--uros-splitter "$g")
        done
    fi
    out=$("$PUYA_SOL" "${args[@]}" 2>&1)
    # bash pattern match avoids SIGPIPE on huge outputs (`echo | grep -q`
    # with pipefail false-positives when grep matches early and echo
    # gets EPIPE).
    if [[ "$out" == *"puya completed successfully"* ]]; then
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
