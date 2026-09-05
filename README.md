# Algorand Solidity

> [!CAUTION]
> **AI-assisted proof-of-concept. Side project. NOT production-ready.**
>
> This is an experimental compiler being built largely through pair-programming with AI coding assistants. It is:
>
> - **Not audited.** No security review has been performed on any part of the toolchain — neither the compiler itself nor any TEAL it emits.
> - **Not officially supported** by the Algorand Foundation or any other organization. This is a personal side project.
> - **Maintained on a best-effort basis.** No guaranteed release cadence. Identified bugs may sit unfixed for long periods of time. That said, Pull requests, issue reports, feature requests, questions, etc. are welcome and encouraged!
> - **A research/PoC effort**, not a stable release. APIs, AWST shapes, codegen patterns, output formats, and even successful test counts can change between commits without notice.
> - **Likely to mis-compile contracts in subtle ways.** A known backend failure and expected failures remain in the semantic suite; passing tests do not establish EVM equivalence. Research runs explicitly accept adaptations such as non-enforced static calls, uncatchable inner-call failures, and AVM-specific address and storage conventions.
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

The full semantic run on **2026-09-05** recorded **1,623 passed, 1 failed,
101 xfailed, and 39 xpassed**. All **16 native tests** passed. The remaining
failure is a known backend optimization bug that drops a required
divide-by-zero revert; the suite is **not fully green**. See the
[test guide and revision-specific baseline](tests/solidity-semantic-tests/README.md).

This repository focuses on the compiler and regression tests. The example-port
collections have been removed in preparation for a separate repository; no
replacement repository is published here yet. Token experiments remain under
[`WIP/tokens/`](WIP/tokens/), and the
[historical replayer](tests/chainwide-historical-diff/README.md) remains in-tree.

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

Use `--import-path` for the source root so explicit files and imports share
solc's normalized source-unit names. For intentional alternate import spellings,
use `--remapping alias=canonical-name` to select the same source unit explicitly.
AWST contract IDs use solc's fully qualified `source-unit:Contract` identity.
Artifact filenames retain the short contract name; distinct deployable
contracts with the same name are rejected
with a collision diagnostic instead of silently overwriting one another.

Every successful frontend run also writes `artifact-manifest.json`, with the
byte length and SHA-256 digest of each recorded compiler artifact. A
`backend-complete` phase is the commit marker for a successful, validated
backend run; `frontend-only` and `frontend-ready` record only validated
frontend output. Compiler-owned files for the current targets are invalidated
before the backend runs, so stale files cannot make a failed run appear
complete.

Source text is passed to the pinned Solidity frontend unchanged by default,
including every version pragma in entry, additional, and imported files. An
incompatible pragma is therefore a compilation error, just as it is in solc.
The old pre-0.8 compatibility transforms remain available only for corpus
research through the explicit `--legacy-source-rewrite` flag. That mode prints
an unsuppressible warning and writes `source-rewrite-manifest.json` containing
the exact original and transformed text plus both Keccak-256 hashes for every
source unit. Its output must not be represented as a compilation of the
original source.

`--evm-memory-layout` is unavailable until a universal EVM memory model is
implemented, and the `--evm-layout` umbrella is unavailable because it would
include that missing behavior. Both options fail with status 2 before source
processing, even when logs are filtered. The implemented storage-only subset
remains available explicitly as `--evm-storage-layout`.

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
`uint256`, and `block.gaslimit` defaults to the group's pooled app-call opcode budget. For
historical replay or EVM-domain compatibility, override them with
`--evm-chain-id <uint256>` and `--evm-block-gas-limit <uint256>`.
`block.coinbase` has no AVM analogue and is a compile error unless an explicit
20-byte value is supplied with `--evm-coinbase <hex-address>`.

Non-exact EVM behavior generally fails compilation by default. Research builds can
acknowledge an individual supported adaptation with a repeatable flag such as
`--allow-divergence block-basefee`; there is deliberately no global “allow
everything” switch. `puya-sol --help` lists the stable names. Configured
environment values are already explicit, while fundamentally unsupported
features remain compile errors.

Static-call read-only enforcement is an accepted exception: `.staticcall()`,
typed external `view`/`pure` calls (including function pointers), and Yul
`staticcall` emit a warning. Cross-contract calls use ordinary inner application
calls and may change state. No `--allow-divergence staticcall` flag is required
(the flag remains accepted for compatibility). Separate divergences, such as
low-level call failure handling, still require their own acknowledgment.

`type(C).creationCode` and `type(C).runtimeCode` are hard compile errors: the
deployed program is TEAL, so EVM bytecode — even solc's real object for the
same source — describes a contract that does not exist on chain, and its usual
consumers (CREATE2 address derivation, code hashing) would silently compute
meaningless values.

The main branch does not automatically split oversized programs. Contracts
that exceed AVM program-size limits must currently be reduced or refactored.

## Testing

The Solidity semantic-test corpus, imported from
`solidity/test/libsolidity/semanticTests/` and extended with local regressions,
drives most of the coverage. This research harness explicitly opts into the
legacy source rewrite and every policy-listed AVM adaptation so it can measure
and classify those differences; ordinary compiler invocations preserve source
text and apply the fidelity policy above. The
[test guide](tests/solidity-semantic-tests/README.md) records the current baseline,
harness setup, and how to retain a machine-readable result for each run.

Run the full suite (requires AlgoKit localnet running):

```bash
cd tests/solidity-semantic-tests
PUYASOL_LOCALNET_RESET=0 pytest tests/ -n 2     # all categories
PUYASOL_LOCALNET_RESET=0 pytest tests/conversions/ -q
PUYASOL_LOCALNET_RESET=0 pytest tests/puyasolRegression/test_builder_findings.py -q -n 2
```

`PUYASOL_LOCALNET_RESET=0` preserves the existing LocalNet ledger during tests.

## Repository layout

| Path | Purpose |
|---|---|
| [`src/`](src/) | C++ frontend — Solidity AST → AWST builder, runner, JSON serializer |
| [`src/libs/AVM.sol`](src/libs/AVM.sol) | Bundled Solidity facade for Algorand-native operations |
| [`tests/solidity-semantic-tests/`](tests/solidity-semantic-tests/) | Solidity semantic-test harness and compiler regressions |
| [`tests/avm-stdlib/`](tests/avm-stdlib/) | Algorand-native standard-library regressions |
| [`tests/chainwide-historical-diff/`](tests/chainwide-historical-diff/) | Historical EVM/AVM differential replayer |
| [`WIP/tokens/`](WIP/tokens/) | Retained token experiments |
| [`solidity/`](solidity/) | Submodule — Solidity compiler frontend (AST + type checker) |
| [`puya/`](puya/) | Submodule — Python AWST → TEAL backend |
| [`build/`](build/) | CMake build output (gitignored) |

The `WIP/` directories contain experiments, not supported compiler interfaces.

## Architecture notes

> Not exhaustive! these are a handful of the load-bearing decisions that shape the codebase. Plenty of other compiler-level conventions (ARC4 selector encoding, modifier inlining, fn-ptr dispatch tables, free-memory-pointer simulation, transient storage layout, etc.) live only in the source. Documenting these is a WIP.

- **AWST is the contract** — puya-sol's job is to emit a well-typed AWST JSON that puya accepts. Test failures often come down to the wrong AWST shape rather than wrong semantics; the AWST round-trip is the primary debugging surface.
- **Storage uses AVM state** — the default named-cell model uses app globals and boxes, including hashed keys for mapping entries. `--evm-storage-layout` instead selects EVM slot-based storage backed by boxes. See `src/builder/storage/StorageMapper.cpp`.
- **Nested-storage keys aren't EVM slot arithmetic** — EVM derives the slot for `m[k1][k2]` via repeated `keccak256(k . slot)`. puya-sol walks the declared type outer-to-inner and classifies each `[i]` level: a **mapping** level — or an **array level whose element type contains a mapping** — contributes a bytes part to a single composite `sha256(...)` box key; an **array level whose element type is "flat"** (no mapping below) becomes an `IndexExpression` applied to the box value after the read. So `mapping(K=>T[N]) q` stores one `T[N]` box per `k` and indexes `i` inside it (1 key part: `k`); `mapping(K=>Y)[N] n` stores one `Y` box per `(i, k)` pair (2 key parts: both `i` and `k`). Per-level encoding is canonical: array-level keys always encode as `itob(uint64)`, mapping-level keys encode as the declared `keyType` (`uint256` → 32-byte left-padded biguint, `uint8`/`uint64` → `itob`, `address`/`bytes` → reinterpret-as-bytes). The auto-getter (`PublicGetterBuilder.cpp`) and the lvalue path (`SolIndexAccessHandlers.cpp::handleMappingAccess`) share this classification so a write through the constructor reads back through the public getter; both sides agree on key bytes even when the call sites pass differently-typed indices. See `src/builder/sol-ast/exprs/SolIndexAccessHandlers.cpp` and `src/builder/contract/PublicGetterBuilder.cpp`.
- **Assembly memory spans scratch slots** — byte ranges and words can cross 4,096-byte slot boundaries. `--evm-memory-slots` controls the bounded region (default: five slots, 20 KiB). This is not a universal EVM memory model for all Solidity values. See `src/builder/ScratchLayout.h` and `src/builder/assembly/MemoryHelpers.cpp`.
- **Inline assembly is supported but limited** — Yul memory/storage operations, arithmetic, user functions, and recognized precompile calls have explicit lowerings in `src/builder/assembly/`. Supported storage behavior depends on the selected profile. EVM-only operations are subject to the same documented fidelity policy; accepting an assembly block does not establish arbitrary EVM equivalence.
- **Contract size limit** — AVM program-size limits still apply, and the main branch has no automatic contract-splitting pass.
- **Solc facts drive lowering** — resolved declarations and types, linearized bases, and call-graph reachability come from solc. Shared builder helpers consume those facts for call dispatch, parameter conventions, and storage access rather than independently re-resolving Solidity semantics.
- **Inheritance is flattened** — the emitted contract includes the required base implementations as subroutines; this does not require runtime delegatecall.
- **No catchable inner-call failures** — try/catch success paths can run with the explicit `try-catch` adaptation, but a failed inner transaction aborts the whole AVM transaction, so catch clauses cannot recover.
- **No CREATE2** — Salted deploys (`new C{salt: …}(…)`) have no AVM analogue (app IDs are assigned by the protocol at create time, not derived from salt+initcode hash) and the entire `saltedCreate/` cluster is unsupported. Plain `new C(...)` works via inner-txn app-create.
- **Delegate calls are unsupported** — AVM has no equivalent of “execute foreign code in my storage context.” Runtime `address(...).delegatecall(bytes)` therefore fails compilation unless its deliberate runtime-failure emulation is explicitly acknowledged; the library-attached form (`using L for *`) works because it resolves to a compile-time subroutine call.
- **Tokens compile to apps, not automatically to ASAs** — ERC20/721/1155 logic retains its own state and methods in an AVM application. Algorand-native asset operations are available explicitly through the bundled standard library; there is no general ERC-to-ASA translation pass.

## Related docs

- [Semantic test guide and baseline](tests/solidity-semantic-tests/README.md)
- [Accepted EVM divergences](EVM_DIVERGENCE.md)
- [Proxy lowering and remaining design work](proxy.md)
- [Open engineering follow-ups](docs/KNOWN_ISSUES.md)
