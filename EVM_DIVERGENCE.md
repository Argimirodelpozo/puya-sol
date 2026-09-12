# Documented EVM divergences

Deliberate, fail-loud or fail-honest differences between puya-sol's AVM
lowering and EVM semantics. Test xfail reasons cite this file; entries here
are POLICY, not bugs.

## Compiler enforcement

Detected non-exact behavior generally fails compilation by default. A research or
compatibility build must acknowledge each eligible behavior separately with
the repeatable `--allow-divergence <name>` option; `puya-sol --help` lists the
stable names, and there is no catch-all opt-in. Explicitly configured EVM
environment values are already acknowledged by their configuration options.
Fundamentally unsupported features remain unconditional compile errors.

Static-call read-only enforcement is an accepted warning-only exception.
Explicit `.staticcall()`, typed external `view`/`pure` calls (including getters
and function pointers), and Yul `staticcall` emit a warning: AVM cross-contract
calls use ordinary inner application calls and may change state. They do not
require `--allow-divergence staticcall`; that token remains valid for older
scripts. This exception does not waive other call-related opt-ins.

The historical `--evm-memory-layout` flag did not actually select a universal
memory model. It and the `--evm-layout` umbrella are now rejected before source
processing; only the implemented `--evm-storage-layout` subset can be enabled.

The semantic-test harness opts into every listed adaptation because its job is
to measure and classify EVM/AVM differences. That harness policy does not alter
the compiler's default policy or add runtime enforcement for accepted adaptations.

## Block seed and calldata conventions

`block.difficulty` and `block.prevrandao` (including their Yul opcodes), when explicitly opted in, read the
Algorand seed at transaction `FirstValid - 1` (zero when FirstValid is zero).
This lies in the AVM block-read window independently of simulation/submission
timing. It is known in advance and the caller can select the validity window:
it is **not secure randomness** and does not reproduce EVM prevrandao.

In the EVM profile, `msg.data` is the selector plus the ABI body only; xchain
claims and other transport metadata are excluded. Native ARC4 retains its
compatibility concatenation of routed selector and ARC4 argument values.
Unknown ARC4 fallback calls recognize only the two-argument raw carrier.
`msg.sig` is the first four bytes, right-zero-padded for short/empty data.
Direct constructor reads have empty calldata and a zero selector.

## Address identity and native value transfer (EVM profile)

The EVM profile (`--contract-abi evm`) gives Solidity one 160-bit address
namespace: calldata addresses decode as 20 bytes zero-extended, `msg.sender`
is normalized to the low 20 bytes of the AVM sender, and mapping keys /
keccak digests use the EVM 32-byte word form. This is what makes storage
layouts, hashes, and signatures byte-exact against EVM ground truth — the
namespace is the coordinate system of the differential-replay certification.

**The projection is lossy.** A padded pseudo-account
(`bzero12 ++ low20`) is not a spendable AVM identity: nobody holds a key
for it. Consequently, **native value transfer** to an ordinary 160-bit identity
without receiver mapping sends funds to a keyless address, unrecoverably. The
EVM profile without an account model is a differential/compat instrument, not
a deployment target. One shared payment boundary covers `transfer`, `send`,
call-value payments (including value-bearing Yul `call`), high-level
`selfdestruct` beneficiaries, and child-application funding. Unproven EVM
address destinations require an account template or explicit
`--allow-divergence native-value-transfer` acknowledgement.

Destinations already identified as applications by typed-call or creation
lowering resolve directly to their native escrow, without that identity
opt-in. Address values using the `bzero24 ++ appId` convention also resolve to
the application's escrow at runtime; a nonexistent application fails the
lookup instead of receiving a payment at its keyless encoding. This convention
applies in both ABI profiles. Zero remains the zero address, not application
ID zero. Every payment amount is checked before narrowing to AVM's uint64.

Direct typed, low-level Solidity, and Yul application calls validate this same
address convention; an unrelated address cannot select an application merely
by sharing its last eight bytes. Invalid or nonexistent targets abort, consistent with the existing
uncatchable inner-call failure adaptation. Metadata queries instead return no
code. Runtime self aliases resolve to the current application for metadata,
but AVM still rejects self inner transactions: only statically resolved self
calls use the existing direct-subroutine rewrite. This does not expand native
payment permissions or waive the xchain/account-mapping opt-in.

`transfer` and `send` to a contract-convention receiver submit a grouped payment
and zero-argument application call, executing `receive()` or `fallback()` even
at zero value. Receiver rejection aborts the group (including `send`, which
cannot catch an inner failure). These calls use AVM fees/opcode budgets, not the
EVM 2,300-gas stipend. Ordinary accounts receive only a payment; child funding
and high-level `selfdestruct` payouts do not invoke receiver code.

**The xchain account model provides a spendable mapping**
(`--xchain-template`, see github.com/algorandfoundation/xchain-accounts): each
20-byte EVM identity E owns the LogicSig account
`A(E) = sha512_256("Program" || template-with-owner-spliced)`, controlled by
the holder of the EVM key. With a pinned template supplied:

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

## Transient words and native addresses

Transient declarations use solc's logical slots and packed offsets. Typed
address, address-UDVT and contract-value reads/writes preserve their full native
32-byte representation: the upper 12 bytes are held in private scratch storage,
outside the logical word. Raw Yul `tload` sees only the canonical packed word;
`tstore` clears the native-only address bytes for declarations in that word,
even if it writes back the same word. Other words retain their native bytes.
This approved adaptation agrees within solc's 160-bit address domain, but a raw
word round trip cannot preserve an arbitrary full-width AVM address.

The logical blob and address shadow are initialized on every application call
and shared by internal subroutine calls. They create no persistent cells.
Declared transient state is limited to five logical words; raw Yul supports
slots 0–127. These are target capacities, not changes to solc's packing rules.

## Bounded buffers, precompiles and call results

Yul memory remains a bounded, scratch-backed address space, configured by
`--evm-memory-slots` (4096 bytes per page). Memory and calldata are separate;
equal offsets do not alias them. Word accesses check the full 32-byte extent,
and byte-range operations preserve partial tails and cross-page data. Operations
that materialize a single AVM byte value are limited to 4096 bytes, even when
the configured memory capacity is larger. Nonempty out-of-range accesses abort;
zero-length memory ranges ignore their unused offset. This is not an unbounded
EVM memory implementation or a redesign of Solidity's typed memory representation.

Solidity and Yul precompile adapters share the byte-level implementation for
constant targets 1, 2, 4, 5, 6, 7 and 8.
Fixed-width cryptographic inputs are right-zero-padded or truncated as required;
BN254 pairing accepts complete 192-byte pairs within the buffer limit, including
the empty product. Modexp is still restricted to 32-byte base, exponent and
modulus fields. Unsupported target implementations, nonzero value sent to a
precompile, and unsupported operand layouts fail loudly. AVM cryptographic or
inner-transaction failures abort the transaction rather than yielding a catchable
EVM call failure. Invalid ecrecover `v`/`r`/`s` ranges return empty bytes; other
recovery failures can still abort in the AVM intrinsic.

Yul calls keep the complete supported result in a separate return-data buffer
and copy only `min(output length, result length)` bytes into memory; the remaining
destination bytes are unchanged. Empty raw Yul application calls invoke
`receive()`/`fallback()`, including value-bearing calls. This differs from the
existing high-level empty value-call adaptation described below. Both precompile
adapters replace the return-data buffer, including when the result is empty.

Modeled external self-calls also replace that buffer, including void calls,
getters and external function-pointer dispatch. The EVM profile and explicitly
EVM-encoded self-call payloads publish EVM ABI bytes; typed ARC4-profile calls
retain their ARC4 return encoding. Genuine internal calls do not replace it.
Fallback/receive handlers publish an explicit empty result when they return
nothing, so a handler's last event cannot be mistaken for return data.
Application result capture accepts only the `0x151f7c75`-prefixed return record;
unprefixed event logs are not return values, including for native ARC4 void calls.
Void public-method routes do not add an empty record: no record means empty
data, while an explicit assembly `return` keeps its actual payload even if
the Solidity declaration has no return parameters.

External function pointers retain the compact application-id/selector layout.
Their full address is checked against the selected profile's application
namespace before compaction, including when decoding an EVM ABI function word.
The zero address is allowed as a pointer value but cannot be invoked. This
namespace restriction is an AVM policy, not an EVM address rule. Opaque pointer
ABI round trips still require `--evm-selectors` to retain the EVM selector.

## Other standing entries (summaries; see tests' xfail reasons)

- Proxy-to-native-update adaptations require the separate, default-off
  `--proxy-adaptation` flag. Without it, proxy names and EIP-1967 slot values
  do not activate body replacements, native admin cells, or update gates.
  See [proxy.md](proxy.md) for the opt-in semantics and recognition limits.

- `delegatecall`: rejected by default because there is no AVM analogue. A
  research build may acknowledge `--allow-divergence delegatecall`, which
  preserves a deliberate runtime-failure lowering rather than fabricating
  foreign execution. Yul `create2`, `selfdestruct`, and metamorphic patterns
  remain hard errors. Dead (solc-pruned) delegatecall is exempt via the
  call-graph reachability gate.
- Address code metadata is a warning-only AVM adaptation, not EVM bytecode
  identity. `.code` returns approval-program bytes; `.code.length` returns
  **allocated program capacity**, not exact length (including for self).
  Solidity metadata and Yul `extcodesize` resolve self and canonical `zero24 ++ uint64(appId)`
  addresses; other forms and missing apps read as no code. Foreign apps must
  be available transaction resources. Program bytes exceeding the AVM stack
  byte-value limit cannot be materialised; the capacity query avoids this.
  During construction only self reads as empty; other deployed apps remain
  queryable, and receiver effects are preserved.
- `.codehash` supports direct self (approval-program hash, empty-code hash
  during construction) and the existing constant zero/precompile convention
  (zero for address 0, empty-code hash for addresses 1–10). These constants
  are not account-existence facts. Other receivers, including arbitrary
  nonzero literals, are compile errors. Yul `extcodehash` remains unsupported;
  The shared size query retains the AVM allocated-capacity adaptation.
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
  low-level-call-outcome`; `staticcall` additionally warns about the missing
  read-only guarantee. Two consequences vs the EVM: a REJECTED call
  aborts the whole transaction (`ok == false` is not catchable), and a
  CODELESS target aborts where the EVM silently succeeds with
  `(true, "")` — fabricating that success would let error handling pass
  spuriously. Zero-value `t.call("")` on a real contract executes the
  callee's `receive()`/`fallback()` like solc (zero-arg app call).
  Solidity `{value:}` + empty calldata stays a bare payment: the receive BODY does
  not run (see the value-transfer section above).
- Default-layout storage uses [versioned holder keys](docs/storage-format.md),
  not EVM slot arithmetic. ARC-56 records exact roots, not prefix maps for
  hash-derived entries. Existing deployments require their original artifacts.
- Indexed DYNAMIC event params and selectors including returns are documented
  wire-level divergences.
