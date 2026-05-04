# AAVE V4 source patches for puya-sol

This directory contains patched copies of AAVE V4 contracts adapted to
compile through puya-sol → puya → AVM. Original sources are unchanged
upstream; the patches here are minimal, surgical, and tagged
`PUYA-SOL PATCH` in the source.

## Why patches?

AVM is not an EVM clone. A handful of Solidity features fundamentally
can't be replicated on AVM with equivalent semantics. Where puya-sol
can transform automatically (overflow checks, ABI encoding, storage
slot derivation), it does. Where it can't, the source must be
rewritten — that's what's documented here.

## Patches in this folder

### 1. try/catch removed (3 files)

**Why:** Solidity's `try/catch` requires in-transaction recovery from a
revert (the catch path runs after the try'd call's frame has been
unwound but before this function returns). AVM has no equivalent: a
failing inner-txn aborts the whole outer txn unconditionally. There is
no way to honor the catch path's user-written logic without changing
program semantics.

**Approach:** Each `try X(...) {} catch {}` site in this codebase used
the empty-catch pattern as a frontrunning-tolerance hint — typically
to swallow `IERC20Permit.permit()` reverts so an attacker who
pre-consumed the user's permit nonce couldn't grief the entrypoint.
We can't replicate the swallow on AVM, so we forward the revert and
document that callers must retry on failure (e.g., via a different
signature, or by detecting the consumed nonce client-side).

**Files patched (all marked `PUYA-SOL PATCH`):**

- `PositionManagerBase.sol`
  - `setUserPositionManagersWithSig` (was line 58–68)
  - `permitReserveUnderlying` (was line 83–93)
- `Spoke.sol`
  - `permitReserveUnderlying` (was line 482–492)
- `TokenizationSpoke.sol`
  - `depositWithPermit` (was line 184–194)

These three files are inherited by the 4 deployable contracts that
exercise the patched paths: `ConfigPositionManager`,
`GiverPositionManager`, `SpokeInstance`, `TokenizationSpokeInstance`.

**Behavior change summary:** an inner permit/sig revert now aborts
the outer transaction instead of being silently swallowed. The change
is observable to integrators that intentionally relied on swallowing.

## Compile status (post-patches, against current puya-sol main)

| Contract | Status | Notes |
|---|---|---|
| `AccessManager` | ✅ compiles | unblocked by [DataOps.cpp `m_haltEmitted` fix](../../../src/builder/assembly/DataOps.cpp) |
| `AccessManagerEnumerable` | ✅ compiles | same fix |
| `PositionManagerBase` | ✅ compiles | abstract; try/catch patch applied |
| `Spoke` | ✅ compiles | abstract; try/catch patch applied |
| `TokenizationSpoke` | ✅ compiles | abstract; try/catch patch applied |
| `ConfigPositionManager` | ❌ FAIL | pre-existing puya-sol bug: `_cachedThis = uint256(uint160(address(this)))` — address→uint256 type cast not emitted, assigns `account` to `biguint` slot |
| `GiverPositionManager` | ❌ same | inherits the same EIP712 base |
| `SpokeInstance` | ❌ same | inherits the same EIP712 base |
| `TokenizationSpokeInstance` | ❌ same | inherits the same EIP712 base |
| `Hub` | ❌ FAIL | pre-existing puya-sol bug: `TupleExpression.wtype` emits `ARC4Struct` (the `Asset` struct from AssetLogic) where puya expects `WTuple` |
| `ERC1967Proxy` | ❌ FAIL | EVM proxy pattern: needs delegatecall semantics + bytecode-level deploy |

## Remaining work to ship full AAVE V4

1. **address → uint256 cast emission** (puya-sol bug). Fix in
   `SolTypeConversion.cpp` to recognize the
   `uint256(uint160(address X))` idiom and emit a
   `ReinterpretCast(account, biguint)` followed by an explicit byte-
   layout match. Unblocks 4 contracts.

2. **TupleExpression with struct value** (puya-sol bug). The
   diagnosis is that `TupleExpression.wtype` is required to be
   `WTuple` by puya, but puya-sol emits the struct's `ARC4Struct`
   wtype directly when a function returns a struct via a tuple
   destructure. Fix is at the call site building the
   `TupleExpression` — wrap the struct in a `WTuple([struct])` or
   emit a `NewStruct` instead. Unblocks `Hub`.

3. **Deploy-size for Hub (16 KB) and SpokeInstance (17 KB)**: use
   `--uros-splitter` (already verified end-to-end on
   `HubConfigurator`).

4. **`ERC1967Proxy`**: AVM has no equivalent of `delegatecall` or
   in-place bytecode replacement during a single txn. Acceptable
   path: drop the proxy pattern entirely for AAVE V4 deployments on
   Algorand, since AVM's UpdateApplication serves the same upgrade
   purpose with native auth. Out of scope for this folder.
