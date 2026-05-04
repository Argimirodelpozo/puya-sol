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

| Contract | Status | Size | Notes |
|---|---|---|---|
| `AccessManager` | ✅ | 5.9 KB | unblocked by [DataOps.cpp `m_haltEmitted` fix](../../../src/builder/assembly/DataOps.cpp) |
| `AccessManagerEnumerable` | ✅ | 9.0 KB | same fix; needs `--uros-splitter` to deploy |
| `PositionManagerBase` | ✅ | abstract | try/catch patch applied |
| `Spoke` | ✅ | abstract | try/catch patch applied |
| `TokenizationSpoke` | ✅ | abstract | try/catch patch applied |
| `ConfigPositionManager` | ✅ | 4.4 KB | unblocked by `account → biguint` conversion in TypeConversions.cpp |
| `GiverPositionManager` | ✅ | 3.8 KB | same fix |
| `TokenizationSpokeInstance` | ✅ | 7.4 KB | same fix |
| `SpokeInstance` | ❌ | — | tuple-arity mismatch in inherited Spoke code: source `(bool, Encoded(...), Encoded(...), Encoded(...), Encoded(...))` assigning to `(bool, Encoded(...))` target — separate puya-sol struct-tuple-destructure bug |
| `Hub` | ❌ | — | `TupleExpression.wtype` emits `ARC4Struct` (the `Asset` struct from AssetLogic) where puya expects `WTuple` |
| `ERC1967Proxy` | ❌ | — | EVM proxy pattern: delegatecall semantics; should be replaced with native UpdateApplication |

## Remaining work to ship full AAVE V4

1. **Tuple-arity destructure mismatch** (puya-sol bug, blocks
   `SpokeInstance`). When a function returning multiple structs is
   destructured into a smaller tuple (e.g., `(success, accountData) =
   _tryX()` where `_tryX` returns 5 values), puya-sol emits the full
   5-tuple as the source of an assignment whose target is the smaller
   2-tuple. The fix is at the destructure site — slice the source
   tuple to match the target arity, or rewrite the assignment as
   per-element copies. Unblocks `SpokeInstance`.

2. **TupleExpression with struct value** (puya-sol bug, blocks
   `Hub`). `TupleExpression.wtype` is required to be `WTuple` by
   puya, but puya-sol emits the struct's `ARC4Struct` wtype directly
   when a function returns a struct via a tuple destructure. Fix is
   at the call site building the `TupleExpression` — wrap the struct
   in a `WTuple([struct])` or emit a `NewStruct` instead.

3. **Deploy-size for Hub (16 KB) and SpokeInstance (17 KB)**: use
   `--uros-splitter` once compile-side fixes land. Verified
   end-to-end on `HubConfigurator`. Also `AccessManagerEnumerable` at
   9 KB needs the splitter.

4. **`ERC1967Proxy`**: AVM has no equivalent of `delegatecall` or
   in-place bytecode replacement during a single txn. Acceptable
   path: drop the proxy pattern entirely for AAVE V4 deployments on
   Algorand, since AVM's UpdateApplication serves the same upgrade
   purpose with native auth. Out of scope for this folder.
