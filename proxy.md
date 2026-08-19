# Proxy patterns on the AVM — analysis and implementation directions

Every EVM proxy pattern is a workaround for one protocol fact: **deployed EVM
bytecode is immutable**. The AVM does not share that fact — an application's
approval program is natively replaceable via an `UpdateApplication`
transaction, with the app id, app account, and all state preserved, and with
the approval program itself deciding who may update. That inverts the design
problem: instead of *simulating the delegatecall mechanism* (which puya-sol
rejects by policy — no shared-storage code borrowing exists on the AVM), we
*lower each pattern's intent* onto native primitives.

This document goes pattern by pattern. "Replay support" refers to the
chainwide-historical-diff harness; "compile support" refers to lowering
user-written proxy code.

Status legend: ✅ implemented · 🔶 designed, not built · 💭 direction only.

---

## 1. EIP-1967 proxy + implementation (the "standard" upgradeable proxy)

**EVM shape.** A thin proxy whose fallback delegatecalls the implementation
address stored at slot `keccak256("eip1967.proxy.implementation") − 1`
(admin at a sibling slot). The odd slots exist only to avoid colliding with
the implementation's storage layout, since proxy and implementation share one
storage space.

**AVM mapping.** The pair collapses to ONE updatable application:

| EVM concept | AVM equivalent |
|---|---|
| proxy address (stable identity) | app id (stable by construction) |
| implementation slot | the approval program itself |
| `upgradeTo(newImpl)` | `UpdateApplication` with new program bytes |
| admin slot + admin gate | the update branch's sender check |
| 1967 slot discipline | unnecessary — one app, one state |
| `initialize()` + initializer latch | unnecessary — create-time code runs once natively |

The one real impedance mismatch: EVM's `upgradeTo` **points at code already
on chain**, while an AVM update **supplies** the program. A faithful lowering
therefore moves the upgrade entry point half off-chain: the admin submits the
newly compiled program in the update transaction instead of calling a method
with an address argument. Operationally this is the same ceremony (compile,
governance-sign, submit) minus one deployment.

**Replay support: ✅** (CCTP v2). `fetch.py --source-from <impl>` splits the
fused roles — source/ABI/compiler from the implementation, history/events/
creation from the proxy. The implementation deploys directly; the initializer
latch (`_disableInitializers()` in the ctor) is neutralized by a printed,
must-hit patch; the historical `initialize(...)` calldata is harvested from
the creation transaction's delegatecall trace and replayed as the first call
(`gen_v2_config.py`). Proxy-admin selectors (`upgradeToAndCall`,
`changeAdmin`, `upgradeTo`) are recognized and skipped — a directly-deployed
implementation has no proxy surface.

**Compile support: ✅ v1** (`src/builder/proxies/Erc1967Lowering`). The three
EIP-1967 slot constants are recognized at asm `sload`/`sstore` sites:
- **admin slot** reads/writes lower to a synthesized biguint app global
  `__erc1967_admin` (unset reads 0, EVM semantics), and any contract that
  touches it grows an ARC-4 `__erc1967_update()` method — declared in
  ARC-56 with `call: ["UpdateApplication"]`, never on create — that asserts
  `Txn.Sender == admin` and emits ARC-28 **`Upgraded(address)`** (EIP-1967's
  event signature; the implementation arg is the app's own identity), so
  indexers see the same upgrade history as on EVM. The update txn carries
  the method selector in `ApplicationArgs[0]`; zero admin ⇒ rejected, and a
  selector-less bare update is rejected too (fail-closed both ways).
  (`AdminChanged`/`BeaconUpgraded` are deliberately NOT synthesized: on EVM
  those are Solidity-level emits — OZ's ERC1967Utils — which compile
  through the normal event path already; a raw asm `sstore` to the admin
  slot emits nothing on EVM and so emits nothing here.)
- **implementation slot** reads lower to the app's own identity
  (`bytes24 ++ itob(app id)`, the contract-value convention); writes
  (`upgradeTo` bodies) lower to a runtime trap whose message explains the
  native-update ceremony. The AVM upgrade is the `UpdateApplication`
  transaction itself, submitted by the admin with the new program bytes.
- **beacon slot** reads/writes trap at runtime (see §4).

Guard: `puyasolRegression/test_erc1967.py` — full flow (fail-closed update,
admin round-trip, upgradeTo trap, admin-signed native update preserving
state) in both default and `--evm-layout` modes. `Proxy.sol`/`ERC1967Utils`
machinery unreached from compiled entry points still DCEs away as before.

**Mid-history upgrades: ✅** (joint replay `"upgrades"` config). At the
historical upgrade block the oracle leg re-registers the app with the next
implementation's compiled program (boxes/globals persist — exactly a native
`UpdateApplication`), the EVM leg swaps the runtime code at the historical
address with storage preserved, and both replay the `upgradeToAndCall`
embedded calldata. Per-implementation artifact dirs (`out_avm_*`) + era-aware
ARC-56/ABI switching; differ decodes both eras' events. Proven by
`selftest_joint_upgrade.py` (synthetic two-era contract; an upgrade-missing
leg diverges in all three lanes). CCTP v1/v2 did not upgrade in-window, so
their configs carry no entries.

---

## 2. Transparent proxy (OpenZeppelin)

**EVM shape.** EIP-1967 slots plus a routing rule in the proxy: the admin's
calls go to the proxy's own admin functions; everyone else falls through to
the implementation. Exists to close the selector-clash hole (an
implementation method whose selector collides with `upgradeTo`).

**AVM mapping.** The routing rule is moot: the "admin surface" is the native
update path, which is a different *transaction type* (`OnCompletion`), not a
selector — selector clashes with upgrade machinery are impossible. Lowering
is identical to §1. The admin check that transparent proxies do in the
fallback becomes the update-branch sender check.

**Replay: ✅** (same as §1 — CCTP v2's `AdminUpgradableProxy` is this
pattern). **Compile: 🔶** subsumed by §1.

---

## 3. UUPS (EIP-1822)

**EVM shape.** The upgrade function lives in the **implementation** (proxy is
minimal); `upgradeTo` writes the 1967 slot "from the inside" and is guarded
by `onlyProxy`/`notDelegated` checks via the immutable `__self` address.

**AVM mapping.** Same collapse as §1, with two extra details:
- `__self`-based checks (`address(this) != __self`) compare the executing
  address against the deploy-time one. On the AVM there is no
  delegated-vs-direct distinction, so the honest lowering is
  `onlyProxy → constant-true`, `notDelegated → constant-true` — both
  reachable-code-preserving constants, worth doing via a recognized-idiom
  fold rather than a blanket rule.
- `_authorizeUpgrade(address)` — the user-defined permission hook — is the
  ONE piece worth carrying over verbatim: its body becomes the update
  branch's permission check. This is the cleanest seam for a compile-mode
  feature: "your `_authorizeUpgrade` is your update gate."

**Replay: ✅** mechanically identical to §1 (the latch patch IS the UUPS
artifact). **Compile: 🔶** — §1 plus the `_authorizeUpgrade` mapping.

---

## 4. Beacon proxies (EIP-1967 beacon slot)

**EVM shape.** N proxies each read their implementation from one shared
beacon contract; upgrading the beacon upgrades all N atomically.

**AVM mapping.** No single-primitive equivalent: N apps have N approval
programs, and there is no protocol-level "all follow that app's program"
indirection. Honest options, in order of fidelity:
1. **Grouped updates**: the operator updates all N apps (16 per atomic
   group; beyond that, sequential batches — briefly non-atomic, which is a
   real, documentable divergence from the beacon's atomicity).
2. **Re-architect** (usually right for clones-via-beacon): N instances →
   ONE app with per-instance state (boxes keyed by instance id / local
   state), where "upgrade all instances" is again a single native update.
3. A dispatcher app that inner-calls a "logic app id" read from state —
   superficially beacon-like, but it is composition, NOT code-borrowing:
   the logic app sees its OWN storage, not the caller's. Only sound when
   the logic is stateless. Generally a trap; documented here to warn
   against it, not to recommend it.

**Replay: 💭** fetchable today per-proxy via §1 (each beacon proxy is just a
proxy whose implementation pointer lives elsewhere — `--source-from` still
works); the beacon-upgrade-mid-history case is the §1 "not covered" item.
**Compile: 💭** option 2 is a design decision per protocol, not a compiler
transform.

---

## 5. EIP-1167 minimal clones

**EVM shape.** 45-byte forwarder bytecode, deployed thousands of times
(factories: Uniswap pairs, vesting escrows, Gnosis Safes pre-1.3). NOT
upgradeable — this is about cheap instances sharing one logic contract,
each with its own storage.

**AVM mapping.** Two honest lowerings:
1. **Literal**: each clone = a fresh app running the SAME compiled program.
   The factory's `createClone()` becomes an inner `ApplicationCall` create
   carrying the (one, shared, already-compiled) program — the puya-sol
   child-contract machinery (`new C(...)` → inner create) already does
   exactly this; a clone factory is `new C()` where C is the logic contract.
   Costs more MBR per instance than 45 bytes of EVM code, but is
   semantically exact. **Compile: mostly ✅ already** — `new C()` works; what
   is missing is recognizing `Clones.clone(impl)`/the 1167 asm blob and
   rewriting it to `new C()` (🔶, a contained idiom-recognition task since
   the 1167 bytecode is a fixed template with the address spliced in).
2. **Idiomatic**: N clones → one app, per-instance boxes/local state. The
   right target when instance count is large (per-user escrows), but it is
   a re-architecture, not a transform.

**Replay: 💭** each clone is fetchable via `--source-from <logic>`; a
factory's full brood needs the multi-instance registry (one case per clone
is fine for spot checks).

---

## 6. EIP-2535 Diamonds

**EVM shape.** One dispatcher holding a selector→facet routing table;
facets are separate contracts delegatecalled per selector. Mostly a
24KB-code-limit escape hatch, secondarily modular upgrades
(`diamondCut` replaces individual selectors).

**AVM mapping.** The AVM sibling already exists in this repo: the **uros
splitter** — one logical contract compiled into a main program + helper
programs with a program-swap dance — is a diamond in everything but name,
built for the same reason (program-size ceiling, 8KB→16KB on AVM). The
honest lowering of a diamond is therefore: flatten all facets into one
logical contract; if it fits 16KB, it is just an app (diamondCut degenerates
to a whole-program update); if it does not fit, the splitter takes over.
Per-selector upgrade granularity survives as "recompile + update" — the
routing table is the compiler's problem, not on-chain state.

**Replay: 💭** needs facet-set flattening at fetch time (source-from per
facet, merged); no case has demanded it yet. **Compile: 💭** flatten +
existing splitter.

---

## 7. Metamorphic contracts (CREATE2 + selfdestruct redeploy)

**EVM shape.** Deploy via CREATE2, `selfdestruct`, redeploy different code
at the SAME address. Effectively deprecated on Ethereum (post-Cancun
`selfdestruct` no longer clears code except same-txn).

**AVM mapping.** None, and none warranted: CREATE2 address derivation and
`selfdestruct` are both already hard errors by design. An app that wants
different code at the same identity is… an app update (§1). Keep as a
documented divergence; the pattern is dead upstream anyway.

**Replay/compile: ✅ as hard errors** (existing policy; `selfdestruct`
xfails, CREATE2 counted skip class).

---

## Cross-cutting notes

- **Storage-collision machinery is dead weight on AVM.** Everything built to
  keep proxy and implementation storage apart (1967 slots, unstructured
  storage libs, `StorageSlot.sol`) protects against a hazard that cannot
  occur in a single-app lowering. Compile-mode work should recognize these
  reads/writes rather than fight them: a 1967-slot read IS "my admin/my
  program" and can fold to the corresponding native fact.
- **Initializers.** The `initializer`/`reinitializer` latches exist because
  proxied contracts cannot run constructors. Direct-deploy lowerings run
  constructors natively; the latch machinery reduces to ordinary
  one-time-flag state and needs no special handling beyond the
  `_disableInitializers` ctor patch (replay) or idiom-fold (compile).
- **The replay harness's proxy playbook** (implemented, CCTP v2):
  `--source-from` split fetch · direct implementation deploy ·
  `_disableInitializers` neutralization · creation-trace config-era harvest
  (`gen_v2_config.py`) · proxy-admin selector skip list. Mid-history
  upgrades are the known gap (§1).
