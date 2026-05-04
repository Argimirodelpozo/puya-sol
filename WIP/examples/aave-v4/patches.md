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
| `HubConfigurator` | ✅ | 5.7 KB | already passing (also splitter dance demo target) |
| `SpokeConfigurator` | ✅ | 5.0 KB | already passing |
| `SpokeInstance` | ✅ | 22.8 KB | unblocked by puya-side patch (see `../../../puyabug.md`); needs `--uros-splitter` to deploy |
| `Hub` | ✅ | 20.0 KB | unblocked by [AWSTBuilder.cpp augmentReturns shape fix](../../../src/builder/AWSTBuilder.cpp); needs `--uros-splitter` to deploy |
| `ERC1967Proxy` | ❌ | — | EVM proxy pattern: delegatecall semantics; should be replaced with native UpdateApplication |

## Remaining work to ship full AAVE V4

1. **Deploy-size for several contracts** — use `--uros-splitter` to
   fit under the 8 KB AVM cap:
   - `SpokeInstance`              22.8 KB
   - `Hub`                        20.0 KB
   - `AccessManagerEnumerable`     9.0 KB
   Splitter dance verified end-to-end on `HubConfigurator`.

2. **`ERC1967Proxy`**: AVM has no equivalent of `delegatecall` or
   in-place bytecode replacement during a single txn. Acceptable
   path: drop the proxy pattern entirely for AAVE V4 deployments on
   Algorand, since AVM's UpdateApplication serves the same upgrade
   purpose with native auth. Out of scope for this folder.

## Tally

12 of 13 deployable contracts now compile (was 5 before this
session). All abstract bases compile too. Only remaining failure
is `ERC1967Proxy` — out of scope by design.
