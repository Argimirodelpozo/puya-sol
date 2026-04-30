# Puya / puya-sol blockers for ctf-exchange v2 tests

This doc tracks bugs in puya-sol (the Solidity → AWST translator) and puya
(the AWST → TEAL backend) that block specific clusters of v2 tests.

Each entry: symptom, root cause, reproduction, and proposed fix. Items are
ordered by leverage (number of tests unblocked).

---

## 1. Constructor: first ARC4 ABI arg's bytes are dropped during validation

**Tests blocked:** 5 admin tests (`test_initial_owner_is_admin`,
`test_transfer_ownership`, `test_add_admin_grants_role`,
`test_pause_unpause_flow`, `test_permissioned_ramp_witness`).

**Symptom:** Owner storage slot reads back all-zero, so every Solady-Ownable
contract's `owner()` returns the zero address and every `onlyOwner` check
reverts.

**Misdiagnosis we ruled out:** The xfail comments blamed Solady's
`shr(96, shl(96, x))` address-cleanup pattern truncating 32-byte addresses
on AVM. That pattern *is* a real issue and I added a peephole at
`src/builder/assembly/CoreTranslation.cpp:282-303` that detects it and
short-circuits to `x` directly. The peephole fires (logged) on every Solady-
using contract — but admin tests still see all-zero owner. So that's not
the root cause.

**Actual root cause:** puya emits ABI argument validation that consumes the
first arg's bytes via `len` without a preceding `dup`, leaving subsequent
references to the first arg reading uninitialized stack/memory.

Reproduction — look at `out/collateral/CollateralOnramp/CollateralOnramp.approval.teal`,
`__postInit` routing:

```teal
__postInit:
    txna ApplicationArgs 1
    len                                    # ← arg1 bytes consumed
    intc_0 // 32
    ==
    assert
    txna ApplicationArgs 2
    dup                                    # ← arg2 kept via dup
    len
    intc_0 // 32
    ==
    assert
    txna ApplicationArgs 3
    dup                                    # ← arg3 kept via dup
    len
    intc_0 // 32
    ==
    assert
    [...constructor body...]
    load 0                                 # ← `newOwner` (mem slot 0)
    intc 4 // 18446744071369603367         # ← _OWNER_SLOT
    bytec_0 // 0x                          # ← VALUE = empty bytes!
    callsub __storage_write
```

Note the asymmetry: `dup` appears before `len` for args 2 and 3, but **not**
for arg 1. The bytes are consumed by `len`, so any later reference to arg 1
has nothing to load.

The downstream effect: Solady's `_initializeOwner(_owner)` writes
`or(newOwner, shl(255, iszero(newOwner)))` to `_OWNER_SLOT`. Without arg 1
loaded into memory, the `newOwner` reference resolves to empty bytes, and
the OR-with-zero-tail evaluates to empty bytes. `__storage_write(slot, b"")`
zeros the slot.

**Proposed fix:** Either add `dup` before the first arg's `len` validation
in puya's ABI router codegen, OR cache each validated arg into a per-arg
memory slot (consistent across all args) so later references don't depend
on stack ordering.

This is a puya backend bug (the `argsValidator`/`load_arg` codegen), not
a puya-sol bug. The fix is in puya proper.

---

## 2. ECDSA recover: solady's `for { 1 } switch case` body collapses to `err`

**Tests blocked:** 1 direct (`test_ecdsa_recover_matches_eth_account`),
plus indirectly all 16 matchOrders body tests + 13 preapproved tests + 6
ERC1271 tests that need EOA signature verification (35 total).

**Symptom:** When solady's `ECDSA.recover(hash, signature)` is invoked,
puya emits TEAL whose body for the EOA signature path is just `err`
(unreachable), so any signature verification reverts unconditionally.

**Reproduction:** Compile any contract that imports `solady/utils/ECDSA.sol`.
Inspect the `_verifyECDSASignature` (or `ECDSA.recover`) routine in the
emitted TEAL. The body of the inline `for { let i := 1 } i { i := 0 }
{ switch ... }` (solady's idiom for early-exit single-iter loops) gets
optimized to `err` because puya's optimizer doesn't recognize the
explicit `break` semantics of `i := 0`.

The ecrecover precompile dispatch path itself works — `staticcall(gas, 1,
ptr, 0x80, 0x00, 0x20)` is correctly recognized in
`src/builder/assembly/CoreTranslation.cpp:445-467` and routed to AVM's
`ecdsa_pk_recover` opcode via `handleEcRecover` in `BuiltinCallables.cpp`.
The bug is upstream of the precompile call: puya's optimizer drops the
control-flow before `staticcall` is even reached.

**Proposed fix (puya):** Recognize the solady `for { let i := 1 } i { i := 0 } { ... }`
pattern as a single-iteration block. Specifically, the optimizer needs to
treat the assignment of `0` to the loop variable as an unconditional exit
rather than dead code that can be folded.

Alternative: write a peephole in puya-sol that detects the pattern at the
Yul level and emits a straight-line block instead of a loop.

This is the most leveraged fix — unlocks ~35 tests that need EOA signature
verification. Without it, every `_validateOrder` call inside `matchOrders`
fails before any business logic runs.

---

## 3. solady SafeTransferLib: assembly `call` to non-constant addr drops the itxn

**Tests blocked:** all matchOrders body tests that depend on USDC transfers
working (i.e. all happy-path matchOrders tests).

**Symptom:** matchOrders' `_matchOrders` calls
`TransferHelper._transferFromERC20(token, from, to, amount)`, which delegates
to solady's `SafeTransferLib.safeTransferFrom`. solady's implementation uses
inline assembly:

```solidity
let success := call(gas(), token, 0, 0x1c, 0x64, 0x00, 0x20)
```

On AVM this should map to `itxn ApplicationCall(token, app_args=[selector,
from, to, amount])`. But puya-sol's expression-context handler at
`src/builder/assembly/CoreTranslation.cpp:445-470` only dispatches when the
address arg is a *constant* (precompile address like 1 for ecrecover).
With a variable `token`, the handler emits `warning: call in pure expression
context with non-constant address — stubbed as success` and returns 1
(success literal) without actually firing any inner-txn.

Reproduction — `out/exchange/CTFExchange/CTFExchange__Helper1/CTFExchange__Helper1.approval.teal`:

```teal
SafeTransferLib.safeTransferFrom:
    proto 4 0
    [...EVM-style memory layout assembly: builds calldata at memory 0x1c...]
    store 0
    retsub                             # ← returns without any itxn
```

The function constructs the calldata blob in TEAL memory and stores it,
then returns. There's no `itxn_begin`/`itxn_submit`. ERC20 transfers
silently no-op.

ERC1155 transfers (CTF) go through plain-Solidity `ERC1155(token)
.safeTransferFrom(...)` instead of inline asm and DO get translated to
`itxn ApplicationCall` correctly.

**Proposed fix (puya-sol):** Add a Yul-level pattern matcher for solady's
`call` form. The calldata is constructed via a sequence of `mstore` ops
that lay down the selector + args at consecutive memory offsets. Walk
backwards from `call(gas, addr, 0, ptr, len, retPtr, retLen)` to recover:

1. The selector (4 bytes at `ptr`).
2. Each arg (32-byte aligned, at `ptr + 4 + 32*i`).

Then emit `itxn.ApplicationCall(app_id=addr, app_args=(selector, arg0,
arg1, ...))`. The retPtr/retLen become a follow-up `gtxns` read of
`LastLog`.

This is the most leverage-per-line-of-puya-sol change available — it
unblocks all of solady's optimized transfer/approve paths, which is the
foundation v2 (and most modern Solidity codebases) builds on.

**Workaround in this branch:** Overwrite v2's `TransferHelper.sol` with a
plain-Solidity variant that does `IERC20(token).transferFrom(...)` directly
through a Solidity interface. puya-sol translates Solidity-interface calls
to `itxn` correctly. Lose ~100 gas per transfer on EVM, gain a working
build on AVM. Documented as `// AVM-PORT-ADAPTATION:` comments in the
file.

---

## 4. hashOrder: cached EIP-712 domain separator write path is broken

**Tests blocked:** 1 direct (`test_hash_order`), plus indirectly any test
that computes order hashes via the cached path (which is most of the
matchOrders + preapproved suite once they're working).

**Symptom:** `hashOrder(order)` always reverts because Solady's EIP-712
mixin reads from a cached domain separator slot that was never written.
The cache write happens in `__postInit` (in solady's `_constructEIP712Domain`
helper). The write path is generated correctly at the Yul level but puya
emits a path that never executes the write.

**Reproduction:** Compile CTFExchange.sol with `--force-delegate matchOrders`.
Inspect the orch's `__postInit` TEAL for any `app_global_put` or
`__storage_write` to a slot that looks like the EIP-712 domain hash cache.
The write is missing (or guarded by an always-false branch).

**Status:** Needs a deeper trace — I haven't isolated which Solidity
construct gets dropped. The likely culprit is the `if (cachedDomainSeparator
== bytes32(0))` initial-write guard, where puya may be evaluating the
condition statically as "already non-zero" because the storage slot's
initial state isn't tracked.

**Proposed fix:** Verify in puya that storage-slot reads of an uninitialized
slot evaluate to zero (matching EVM semantics), not "unknown" (which would
let the optimizer assume the slot is non-zero and drop the write).

This is a puya backend bug (storage state-tracking in the optimizer), not
a puya-sol bug.

---

## Summary of where fixes need to land

| Bug | Component | Tests unblocked |
|---|---|---|
| ABI arg validation drops first arg's bytes | puya (router codegen) | 5 admin tests |
| ECDSA recover body collapses to `err` | puya (optimizer, switch-case in for-loop) | 35 (matchOrders + preapproved + ERC1271) |
| EIP-712 domain cache write dropped | puya (optimizer, storage state-tracking) | ≥1 (more once matchOrders is live) |

**puya-sol changes that already landed in this branch** (correct, but
insufficient on their own to unblock the above clusters):

- `src/builder/assembly/CoreTranslation.cpp` — `shr(96, shl(96, x)) → x`
  peephole for Solady's address-cleanup pattern.
- `src/builder/assembly/CoreTranslation.cpp` — expression-context dispatch
  for `staticcall(_, 1, _, _, _, _)` and `call(_, 1, _, _, _, _)` to the
  ecrecover precompile handler. (Was statement-context only.)
- `src/builder/assembly/MemoryOps.cpp` — `tryHandleBytesMemoryLength`
  peephole for `mload(bytes_var)` reading the length header.
- `src/builder/ContractBuilder.cpp` — lifted the
  `_func.modifiers().empty()` gate on the biguint→ARC4UIntN(N) ABI remap
  so methods with `onlyAdmin`-style modifiers correctly emit uint256 ABI
  shape (was emitting uint512 for any modifier-decorated method).
- `src/splitter/SimpleSplitter.cpp` — closure pull-in dedupe fix; injection
  of `__delegate_update` on delegate helpers; biguint padding for cross-
  helper inner-call args.
- `src/json/OptionsWriter.cpp` — `target_avm_version` bumped to 12 to
  match the AVM features lonely-chunk's `op.Box.create` /
  `itxn.ApplicationCall(approval_program=tuple)` need.
