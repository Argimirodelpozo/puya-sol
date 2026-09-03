# Algorand Solidity

> [!CAUTION]
> **AI-assisted proof-of-concept. Side project. NOT production-ready.**
>
> This is an experimental compiler being built largely through pair-programming with AI coding assistants (Claude). It is:
>
> - **Not audited.** No security review has been performed on any part of the toolchain — neither the compiler itself nor any TEAL it emits.
> - **Not officially supported** by the Algorand Foundation or any other organization. This is a personal side project.
> - **Maintained on a best-effort basis.** No guaranteed release cadence. Identified bugs may sit unfixed for long periods of time. That said, Pull requests, issue reports, feature requests, questions, etc. are welcome and encouraged!
> - **A research/PoC effort**, not a stable release. APIs, AWST shapes, codegen patterns, output formats, and even successful test counts can change between commits without notice.
> - **Likely to mis-compile contracts in subtle ways.** ~18% of the upstream Solidity semantic tests still fail or compile-error, and some real-world ports rely on workarounds, in-tree test patches, or features that diverge from EVM semantics (e.g., ARC4 selectors by default and always at the AVM routing boundary, AVM box layout instead of EVM storage slots, no try/catch).
> - **Not production money safe.** Do not deploy compiler output to MainNet, do not handle real funds with anything emitted by this tool, and do not assume security properties of the original Solidity contracts carry over to the TEAL output.
>
> Use at your own risk. Use this for experimentation, prototyping, or research. Do not use it for anything that touches user funds, real assets, or production systems.

---

This is an evolution of a small Solidity-to-Algorand proof-of-concept I sketched out last year — a few hand-translated contracts and a thin script wrapping the Solidity AST + a couple of TEAL templates, mostly to see whether the round-trip was even worth trying. The current codebase is a much wider rewrite: a real C++ frontend that walks the full Solidity AST (via the `solidity` submodule), an explicit AWST builder that emits the same IR `puya` produces from native Algorand-Python, and a regression harness that runs the upstream Solidity semantic-test corpus end-to-end on AVM localnet. So while the PoC answered "is this _possible_?", this iteration is trying to answer "how far can it actually go?".

Solidity → AVM (Algorand) compiler. Translates `.sol` source through Solidity's frontend to AWST (Puya compiler's tree-shaped entry IR), then hands off to [`puya`](https://github.com/algorandfoundation/puya) for AWST → TEAL lowering.

The pipeline:

```
.sol  ──[ puya-sol ]──▶  AWST JSON  ──[ puya ]──▶  TEAL + ARC-56
```

## Status

**1083 / 1322 (82%)** Solidity semantic tests passing as of the latest version. See [`tests/solidity-semantic-tests/`](tests/solidity-semantic-tests/).

Real-world ports compiling and running on AVM localnet (under [`WIP/examples/`](WIP/examples/)):

- **Uniswap V2** (full AMM) and **V4** (361/411 tests passing)
- **OpenZeppelin** v5.0.0 — ERC20/721/1155, AccessControl, Ownable, Pausable, governance, vesting, and ~140 contracts in total
- **AAVE V4** — 32/36 contracts compile
- **Solmate** — ERC20/721/1155/6909, RolesAuthority
- **Morpho Blue** — singleton lending market (111 tests, 4 xfail)
- **SushiSwap V2** — Uniswap V2 fork DEX (32 tests, 1 xfail)
- **Compound V2** — money-market core (23 tests, 5 xfail)
- **MakerDAO Dai**, **Compound Timelock**, **Synthetix StakingRewards**, **Tornado Cash**, **PRB-Math UD60x18**, **WETH9**, **DappHub DSToken/DSGuard**
- **Custom small contracts** — Governance, Timelock, MultiSig, Vesting, Staking pools (each with their own pytest suite)

## Building

Requirements: Git, a C++20 compiler, CMake 3.16 or newer, and Boost 1.83 or
newer with the `filesystem`, `program_options`, and `unit_test_framework`
libraries. Initialize the pinned source dependencies first:

```bash
git submodule update --init --recursive
```

Build the C++ frontend:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

If Boost 1.83+ is installed outside the normal search paths, select its root
explicitly during configuration:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBOOST_ROOT=/path/to/boost-1.83
```

CMake verifies that `solidity` and all of its nested submodules are clean and
at the commits recorded by this checkout. It then configures and builds the
required Solidity libraries under `build/solidity/`; artifacts in the source
submodule (for example `solidity/build/`) are never linked. Before linking
`puya-sol`, the build also verifies that Solidity's generated `BuildInfo.h`
identifies the pinned commit. A mismatch fails closed with an actionable error.
The bundled Solidity standard library is staged under `build/share/puya-sol/`,
so builds remain usable when `-B` points outside the source tree. To install
both the compiler and standard library:

```bash
cmake --install build --prefix /desired/prefix
```

The clean-build CI workflow repeats the recursive checkout, Release build, and
CTest run without pre-existing caches, then uploads
`build/puya-sol-build-manifest.txt`. The manifest records the root and recursive
submodule revisions, toolchain and Boost identity, and SHA-256 hashes of the
compiler and bundled standard library. Generate it locally with
`cmake --build build --target puya-sol-build-manifest`.

Set up the Python backend venv (puya is a submodule at [`puya/`](puya/)):

```bash
cd puya && uv sync
```

This produces:

- `build/puya-sol` — the C++ Solidity-to-AWST frontend
- `puya/.venv/bin/puya` — the Python AWST-to-TEAL backend

## Compiling a contract

```bash
build/puya-sol \
  --source path/to/Contract.sol \
  --output-dir out \
  --puya-path puya/.venv/bin/puya
```

For multi-source projects (e.g., contracts with imports), pass each `--source` repeatedly. Outputs land in the `--output-dir` as `<Contract>.approval.teal`, `<Contract>.clear.teal`, `<Contract>.arc56.json`, plus `awst.json` for debugging.

Add `--evm-selectors` when Solidity-visible selector values must match solc/EVM
keccak semantics. ARC-4 selectors remain the AVM application-call routing
identity, and the compiler translates them at Solidity-visible boundaries such
as `msg.sig`. This mode changes the internal external-function-pointer encoding
from 12 to 16 bytes so it can retain both the Solidity selector and ARC-4 route;
all contracts that exchange such pointers must be compiled with the same mode.

Use `--contract-abi evm` when the AVM application boundary itself should carry
Solidity ABI data. Calls then use `ApplicationArgs[0]` for the 4-byte keccak
selector and `ApplicationArgs[1]` for one canonical EVM ABI argument body;
returns use the usual four-byte AVM return-log carrier followed by canonical EVM
ABI data. Constructor creation carries one canonical body in `ApplicationArgs[0]`.
The default remains `--contract-abi arc4`. Solidity `abi.encode*` and
`abi.decode` are canonical EVM operations in either profile. Explicit ARC4
value encoding is exposed by the bundled standard library without modifying
the Solidity language:

```solidity
import {ARC4} from "libs/AVM.sol";

bytes memory wire = ARC4.encode(abi.encode(a, b));
(uint16 x, address y) =
    abi.decode(ARC4.decode(wire), (uint16, address));
```

The nested `abi.encode` and `ARC4.decode` calls are compiler-recognised type
envelopes; they do not perform an intermediate EVM ABI encode/decode. Using
either ARC4 helper outside these shapes is a compile error. Wire integer widths
follow the resolved Solidity types; use an explicit cast such as `uint16(1)`
when encoding a literal at a particular width.

EVM-only environment values are never supplied as unexplained test constants.
`block.chainid` defaults to the Algorand `GenesisHash` interpreted as a
`uint256`, and `block.gaslimit` defaults to the current AVM opcode budget. For
historical replay or EVM-domain compatibility, override them with
`--evm-chain-id <uint256>` and `--evm-block-gas-limit <uint256>`.
`block.coinbase` has no AVM analogue and is a compile error unless an explicit
20-byte value is supplied with `--evm-coinbase <hex-address>`.

Non-exact EVM behavior fails compilation by default. Research builds can
acknowledge an individual supported adaptation with a repeatable flag such as
`--allow-divergence block-basefee`; there is deliberately no global “allow
everything” switch. `puya-sol --help` lists the stable names. Configured
environment values are already explicit, while fundamentally unsupported
features remain compile errors.

`type(C).creationCode` and `type(C).runtimeCode` are hard compile errors: the
deployed program is TEAL, so EVM bytecode — even solc's real object for the
same source — describes a contract that does not exist on chain, and its usual
consumers (CREATE2 address derivation, code hashing) would silently compute
meaningless values.

The main branch does not automatically split oversized programs. Contracts
that exceed AVM program-size limits must currently be reduced or refactored.

## Testing

The Solidity semantic-test corpus (~1322 tests imported from `solidity/test/libsolidity/semanticTests/`) drives most of the regression coverage. Each iteration's results are captured in [`tests/solidity-semantic-tests/results_v<N>.txt`](tests/solidity-semantic-tests/) so regressions are caught test-by-test. This research harness explicitly opts into every policy-listed AVM adaptation so it can measure and classify those differences; ordinary compiler invocations remain fail-closed.

Run the full suite (requires AlgoKit localnet running):

```bash
cd tests/solidity-semantic-tests
python3 run_tests.py                           # all categories, ~45 min
python3 run_tests.py --category storage         # one category
python3 run_tests.py --file tests/foo/bar.sol   # single file
```

WIP/examples/ ports each have their own `pytest` suite under `<example>/test/`:

```bash
python3 -m pytest WIP/examples/uniswap-v2/test/
python3 -m pytest WIP/examples/openzeppelin/test/
```

Some example suites depend on pre-compiled `out/` artifacts — re-run their compile script (where present) to regenerate.

## Repository layout

| Path | Purpose |
|---|---|
| [`src/`](src/) | C++ frontend (~54 K lines) — Solidity AST → AWST builder, runner, JSON serializer |
| [`tests/solidity-semantic-tests/`](tests/solidity-semantic-tests/) | Solidity semantic-test harness + per-version `results_v<N>.txt` |
| [`WIP/examples/`](WIP/examples/) | Real-world ecosystem ports (Uniswap, OZ, AAVE, …) used for end-to-end coverage |
| [`solidity/`](solidity/) | Submodule — Solidity compiler frontend (AST + type checker) |
| [`puya/`](puya/) | Submodule — Python AWST → TEAL backend |
| [`build/`](build/) | CMake build output (gitignored) |

The `WIP/` prefix marks code that's exercised but still iterating — examples that compile and pass tests but where the surface area is broader than what the upstream `solidity/test/libsolidity/semanticTests/` corpus covers.

## Architecture notes

> Not exhaustive! these are a handful of the load-bearing decisions that shape the codebase. Plenty of other compiler-level conventions (ARC4 selector encoding, modifier inlining, fn-ptr dispatch tables, free-memory-pointer simulation, transient storage layout, etc.) live only in the source. Documenting these is a WIP.

- **AWST is the contract** — puya-sol's job is to emit a well-typed AWST JSON that puya accepts. Test failures often come down to the wrong AWST shape rather than wrong semantics; the AWST round-trip is the primary debugging surface.
- **Storage maps to box state** — Solidity mappings/arrays/structs live in AVM **boxes** (one box per top-level state var, with sha256-derived keys for mapping entries). See `src/builder/storage/StorageMapper.cpp`.
- **Nested-storage keys aren't EVM slot arithmetic** — EVM derives the slot for `m[k1][k2]` via repeated `keccak256(k . slot)`. puya-sol walks the declared type outer-to-inner and classifies each `[i]` level: a **mapping** level — or an **array level whose element type contains a mapping** — contributes a bytes part to a single composite `sha256(...)` box key; an **array level whose element type is "flat"** (no mapping below) becomes an `IndexExpression` applied to the box value after the read. So `mapping(K=>T[N]) q` stores one `T[N]` box per `k` and indexes `i` inside it (1 key part: `k`); `mapping(K=>Y)[N] n` stores one `Y` box per `(i, k)` pair (2 key parts: both `i` and `k`). Per-level encoding is canonical: array-level keys always encode as `itob(uint64)`, mapping-level keys encode as the declared `keyType` (`uint256` → 32-byte left-padded biguint, `uint8`/`uint64` → `itob`, `address`/`bytes` → reinterpret-as-bytes). The auto-getter (`PublicGetterBuilder.cpp`) and the lvalue path (`SolIndexAccessHandlers.cpp::handleMappingAccess`) share this classification so a write through the constructor reads back through the public getter; both sides agree on key bytes even when the call sites pass differently-typed indices. See `src/builder/sol-ast/exprs/SolIndexAccessHandlers.cpp` and `src/builder/contract/PublicGetterBuilder.cpp`.
- **Memory is a scratch-slot blob** — EVM's `memory` model is simulated via a 4096-byte byte-blob in scratch slot 0; `mload` / `mstore` lower to `extract3` / `replace3` against that blob. See `src/builder/assembly/MemoryHelpers.cpp`.
- **Inline assembly is supported but limited** — Yul blocks (`assembly { ... }`) lower opcode-by-opcode where there's a sensible AVM mapping (`mload`/`mstore`, `keccak256`, `sload`/`sstore` for static slots, `add`/`mul`/`shl`/`shr`/signed ops, `caller`/`origin`/`selfbalance`, the precompile addresses, etc.) and several Yul-specific patterns (fn-ptr `.selector`/`.address`, free-memory-pointer arithmetic, storage-pointer aliasing, recursive Yul user functions promoted to subroutines) have explicit codegen. But coverage is far from complete: dynamic-offset `keccak256`, raw `delegatecall`, EVM-storage-slot arithmetic on mapping/array layouts, low-level `create`/`create2`, and several precompiles are stubbed or unsupported. Anything beyond the patterns the upstream `inlineAssembly/` semantic tests exercise is best treated as untested. See `src/builder/assembly/`.
- **Contract size limit** — AVM program-size limits still apply, and the main branch has no automatic contract-splitting pass.
- **Inheritance is flattened** — Solidity's C3 linearization is collapsed at compile time so the emitted contract has all base methods inlined under their MRO names; no runtime delegatecall.
- **No try/catch** — AVM has no analogue for EVM revert-bubbling, so the entire `tryCatch/` semantic-test cluster (20 tests) is currently unsupported.
- **No CREATE2** — Salted deploys (`new C{salt: …}(…)`) have no AVM analogue (app IDs are assigned by the protocol at create time, not derived from salt+initcode hash) and the entire `saltedCreate/` cluster is unsupported. Plain `new C(...)` works via inner-txn app-create.
- **Delegate calls are unsupported** — AVM has no equivalent of “execute foreign code in my storage context.” Runtime `address(...).delegatecall(bytes)` therefore fails compilation unless its deliberate runtime-failure emulation is explicitly acknowledged; the library-attached form (`using L for *`) works because it resolves to a compile-time subroutine call.
- **Tokens compile to apps, not ASAs (for now)** — ERC20/721/1155 contracts are translated faithfully into AVM smart-contract apps with their own balance maps and transfer logic, the same way they live on EVM. This makes the upstream tests round-trip cleanly but ignores Algorand's biggest token-related feature: **ASAs** (Algorand Standard Assets) are first-class tokens at the protocol level, so things like balance lookups, transfers, freeze/clawback, and opt-in flows are all single opcodes / inner-txn fields rather than app calls. A future version will have native ERC20/721/1155 support and lower them onto an ASA created by the constructor; `transfer`/`balanceOf`/etc. become inner asset transfers and `acct_params_get AcctAssetBalance` reads, which is cheaper, composes natively with wallets and DEXes, and gets the security/UX properties of native assets for free. The smart-contract path stays as the fallback for tokens that need behavior ASAs don't expose (e.g. arbitrary `_beforeTokenTransfer` hooks, custom voting/snapshot logic).

## Related docs

- [`tests/solidity-semantic-tests/CURRENT.md`](tests/solidity-semantic-tests/CURRENT.md): living per-version progress log of the semantic testsuite.
