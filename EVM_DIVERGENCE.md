# Documented EVM divergences

Deliberate, fail-loud or fail-honest differences between puya-sol's AVM
lowering and EVM semantics. Test xfail reasons cite this file; entries here
are POLICY, not bugs.

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
unrecoverably. The compiler warns at every such lowering. The EVM profile
without an account model is a differential/compat instrument, not a
deployment target.

**The xchain account model closes this** (`--xchain-template`, see
github.com/algorandfoundation/xchain-accounts): each 20-byte EVM identity E
owns the LogicSig account `A(E) = sha512_256("Program" ||
template-with-owner-spliced)`, controlled by the holder of the EVM key.
With a pinned template supplied:

- payments to a 160-bit identity route to `A(E)` — a real, spendable
  account (on-chain derivation; no registry);
- a caller that IS `A(E)` may claim its owner: `ApplicationArgs[2]` carries
  the 20-byte owner, the entry arm asserts the derived address equals
  `Txn.Sender`, and `msg.sender` adopts the claim — a true EVM identity;
- unclaimed callers keep the low-20 projection as a compatibility shim
  (deploy/creator paths); their identities remain non-payable.

The template must be PINNED: the derived address is the exact program
hash, so a template upgrade changes every account (migration event).
Residual edge: an EVM identity with 12 leading zero bytes is
indistinguishable from the `bzero24 ++ appId` contract-value convention
(probability ~2^-96; such a receiver is treated as a contract).

## Other standing entries (summaries; see tests' xfail reasons)

- `delegatecall` / Yul `create2` / `selfdestruct` / metamorphic patterns:
  compile-time hard errors (no AVM analogue; stubbing would silently
  compute wrong results). Dead (solc-pruned) delegatecall is exempt via
  the call-graph reachability gate.
- `address(other).code`, `extcodesize/extcodehash` of arbitrary addresses:
  hard errors — an arbitrary address cannot be dereferenced to code on the
  AVM; `address.code` of a KNOWN app resolves via the app id convention.
- try/catch catch-clauses: unreachable — a failing inner txn aborts the
  whole transaction (success paths are equivalence-tested).
- Indexed DYNAMIC event params, ARC-56 mapping-prefix, selector-includes-
  returns: documented wire-level divergences.
