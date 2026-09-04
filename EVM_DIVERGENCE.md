# Documented EVM divergences

Deliberate, fail-loud or fail-honest differences between puya-sol's AVM
lowering and EVM semantics. Test xfail reasons cite this file; entries here
are POLICY, not bugs.

## Compiler enforcement

Detected non-exact behavior fails compilation by default. A research or
compatibility build must acknowledge each eligible behavior separately with
the repeatable `--allow-divergence <name>` option; `puya-sol --help` lists the
stable names, and there is no catch-all opt-in. Explicitly configured EVM
environment values are already acknowledged by their configuration options.
Fundamentally unsupported features remain unconditional compile errors.

The historical `--evm-memory-layout` flag did not actually select a universal
memory model. It and the `--evm-layout` umbrella are now rejected before source
processing; only the implemented `--evm-storage-layout` subset can be enabled.

The semantic-test harness opts into every listed adaptation because its job is
to measure and classify EVM/AVM differences. That harness policy does not alter
the compiler's fail-closed default.

## Address identity and native value transfer (EVM profile)

The EVM profile (`--contract-abi evm`) gives Solidity one 160-bit address
namespace: calldata addresses decode as 20 bytes zero-extended, `msg.sender`
is normalized to the low 20 bytes of the AVM sender, and mapping keys /
keccak digests use the EVM 32-byte word form. This is what makes storage
layouts, hashes, and signatures byte-exact against EVM ground truth — the
namespace is the coordinate system of the differential-replay certification.

**The projection is lossy.** A padded pseudo-account
(`bzero12 ++ low20`) is not a spendable AVM identity: nobody holds a key
for it. Consequently, **native value transfer** (`transfer`/`send`/
`call{value: ...}`) to a 160-bit identity sends funds to a keyless address,
unrecoverably. The EVM profile without an account model is a differential/
compat instrument, not a deployment target. The shared high-level payment path
rejects this transfer unless it is explicitly acknowledged with
`--allow-divergence native-value-transfer`. Known gap: `selfdestruct` and an
inline-assembly value-bearing `call` currently build payments outside that
shared boundary, so neither the policy nor receiver mapping covers them.

**The xchain account model provides a spendable mapping**
(`--xchain-template`, see github.com/algorandfoundation/xchain-accounts): each
20-byte EVM identity E owns the LogicSig account
`A(E) = sha512_256("Program" || template-with-owner-spliced)`, controlled by
the holder of the EVM key. For paths using the shared payment boundary, with a
pinned template supplied:

- payments to a 160-bit identity route to `A(E)` — a real, spendable
  account (on-chain derivation; no registry);
- a caller that IS `A(E)` may claim its owner: `ApplicationArgs[2]` carries
  the 20-byte owner, the entry arm asserts the derived address equals
  `Txn.Sender`, and `msg.sender` adopts the claim — a true EVM identity;
- unclaimed callers keep the low-20 projection as a compatibility shim
  (deploy/creator paths); their identities remain non-payable.

The template must be PINNED by the deployment profile: the derived address is
the exact program hash, so a template upgrade changes every account (migration
event). The compiler validates hex, length, and unique placeholder placement;
it does not currently enforce a canonical/audited template hash.
Residual edge: an EVM identity with 12 leading zero bytes is
indistinguishable from the `bzero24 ++ appId` contract-value convention
(probability ~2^-96; such a receiver is treated as a contract).

## Other standing entries (summaries; see tests' xfail reasons)

- `delegatecall`: rejected by default because there is no AVM analogue. A
  research build may acknowledge `--allow-divergence delegatecall`, which
  preserves a deliberate runtime-failure lowering rather than fabricating
  foreign execution. Yul `create2`, `selfdestruct`, and metamorphic patterns
  remain hard errors. Dead (solc-pruned) delegatecall is exempt via the
  call-graph reachability gate.
- `address(other).code`, `extcodesize/extcodehash` of arbitrary addresses:
  hard errors — an arbitrary address cannot be dereferenced to code on the
  AVM; `address.code` of a KNOWN app resolves via the app id convention.
- `address.balance` is denominated in microAlgos, not wei, and requires
  `--allow-divergence address-balance-units`.
- try/catch catch-clauses: unreachable — a failing inner txn aborts the whole
  transaction. Compilation requires `--allow-divergence try-catch` (success
  paths are equivalence-tested).
- `this.f()`: the AVM forbids an app calling itself (no reentrancy), so it
  lowers to a SUBROUTINE call — inside `f`, `msg.sender` and `msg.value`
  keep the ORIGINAL transaction's values, where the EVM's real external
  call would show `msg.sender == address(this)` and the explicitly sent
  value (default 0). Compilation requires `--allow-divergence self-call`.
  Guarded by test_itxn_parity_matrix (oracle answers pinned in the test header).
- Reentrancy in general: the AVM rejects any inner call into an app that
  is already executing (A→B→A aborts), where the EVM allows it. Contracts
  RELYING on reentrancy cannot be expressed; reentrancy-guarded code is
  unaffected.
- Low-level calls (`t.call`/`staticcall`, any calldata incl. empty): submit a
  real inner app call and require `--allow-divergence
  low-level-call-outcome`; `staticcall` additionally requires
  `--allow-divergence staticcall`. Two consequences vs the EVM: a REJECTED call
  aborts the whole transaction (`ok == false` is not catchable), and a
  CODELESS target aborts where the EVM silently succeeds with
  `(true, "")` — fabricating that success would let error handling pass
  spuriously. Zero-value `t.call("")` on a real contract executes the
  callee's `receive()`/`fallback()` like solc (zero-arg app call).
  `{value:}` + empty calldata stays a bare payment: the receive BODY does
  not run (see the value-transfer section above).
- Indexed DYNAMIC event params, ARC-56 mapping-prefix, selector-includes-
  returns: documented wire-level divergences.
