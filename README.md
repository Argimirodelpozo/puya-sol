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
> - **Likely to mis-compile contracts in subtle ways.** ~18% of the upstream Solidity semantic tests still fail or compile-error, and some real-world ports rely on workarounds, in-tree test patches, or features that diverge from EVM semantics (e.g., ARC4 selectors instead of keccak256, AVM box layout instead of EVM storage slots, no try/catch).
> - **Not production money safe.** Do not deploy compiler output to MainNet, do not handle real funds with anything emitted by this tool, and do not assume security properties of the original Solidity contracts carry over to the TEAL output.
>
> Use at your own risk. Use this for experimentation, prototyping, or research. Do not use it for anything that touches user funds, real assets, or production systems.

---

Solidity → AVM (Algorand) compiler. Translates `.sol` source through Solidity's frontend to AWST (Puya compiler's tree-shaped entry IR), then hands off to [`puya`](https://github.com/algorandfoundation/puya) for AWST → TEAL lowering.

The pipeline:

```
.sol  ──[ puya-sol ]──▶  AWST JSON  ──[ puya ]──▶  TEAL + ARC-56
```

## Status

**1083 / 1322 (82%)** Solidity semantic tests passing as of the latest version. See [`tests/solidity-semantic-tests/CURRENT.md`](tests/solidity-semantic-tests/CURRENT.md) for the running changelog.

Real-world ports compiling and running on AVM localnet (under [`WIP/examples/`](WIP/examples/)):

- **Uniswap V2** (full AMM) and **V4** (361/411 tests passing)
- **OpenZeppelin** v5.0.0 — ERC20/721/1155, AccessControl, Ownable, Pausable, governance, vesting, and ~140 contracts in total
- **AAVE V4** — 32/36 contracts compile
- **Solmate** — ERC20/721/1155/6909, RolesAuthority
- **MakerDAO Dai**, **Compound Timelock**, **Synthetix StakingRewards**, **Tornado Cash**, **PRB-Math UD60x18**, **WETH9**, **DappHub DSToken/DSGuard**, and others

## Building

Submodules first:

```bash
git submodule update --init --recursive
```

Build the C++ frontend (Solidity 0.8.x AST library is built as part of the `solidity` submodule):

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-w"
make -j$(nproc)
```

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

For contracts that exceed the 8 KB AVM program-size limit, add `--split-contracts --allow-mid-function-split`.

## Testing

The Solidity semantic-test corpus (~1322 tests imported from `solidity/test/libsolidity/semanticTests/`) drives most of the regression coverage. Each iteration's results are captured in [`tests/solidity-semantic-tests/results_v<N>.txt`](tests/solidity-semantic-tests/) so regressions are caught test-by-test.

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
| [`src/`](src/) | C++ frontend (~54 K lines) — Solidity AST → AWST builder, splitter, runner, JSON serializer |
| [`tests/solidity-semantic-tests/`](tests/solidity-semantic-tests/) | Solidity semantic-test harness + per-version `results_v<N>.txt` |
| [`WIP/examples/`](WIP/examples/) | Real-world ecosystem ports (Uniswap, OZ, AAVE, …) used for end-to-end coverage |
| [`solidity/`](solidity/) | Submodule — Solidity compiler frontend (AST + type checker) |
| [`puya/`](puya/) | Submodule — Python AWST → TEAL backend |
| [`build/`](build/) | CMake build output (gitignored) |

The `WIP/` prefix marks code that's exercised but still iterating — examples that compile and pass tests but where the surface area is broader than what the upstream `solidity/test/libsolidity/semanticTests/` corpus covers.

## Architecture notes

> Not exhaustive! these are a handful of the load-bearing decisions that shape the codebase. Plenty of other compiler-level conventions (ARC4 selector encoding, modifier inlining, fn-ptr dispatch tables, free-memory-pointer simulation, transient storage layout, contract splitter's decisions, etc.) live only in the source. Documenting these is a WIP.

- **AWST is the contract** — puya-sol's job is to emit a well-typed AWST JSON that puya accepts. Test failures often come down to the wrong AWST shape rather than wrong semantics; the AWST round-trip is the primary debugging surface.
- **Storage maps to box state** — Solidity mappings/arrays/structs live in AVM **boxes** (one box per top-level state var, with sha256-derived keys for mapping entries). See `src/builder/storage/StorageMapper.cpp`.
- **Memory is a scratch-slot blob** — EVM's `memory` model is simulated via a 4096-byte byte-blob in scratch slot 0; `mload` / `mstore` lower to `extract3` / `replace3` against that blob. See `src/builder/assembly/MemoryHelpers.cpp`.
- **Inline assembly is supported but limited** — Yul blocks (`assembly { ... }`) lower opcode-by-opcode where there's a sensible AVM mapping (`mload`/`mstore`, `keccak256`, `sload`/`sstore` for static slots, `add`/`mul`/`shl`/`shr`/signed ops, `caller`/`origin`/`selfbalance`, the precompile addresses, etc.) and several Yul-specific patterns (fn-ptr `.selector`/`.address`, free-memory-pointer arithmetic, storage-pointer aliasing, recursive Yul user functions promoted to subroutines) have explicit codegen. But coverage is far from complete: dynamic-offset `keccak256`, raw `delegatecall`, EVM-storage-slot arithmetic on mapping/array layouts, low-level `create`/`create2`, and several precompiles are stubbed or unsupported. Anything beyond the patterns the upstream `inlineAssembly/` semantic tests exercise is best treated as untested. See `src/builder/assembly/`.
- **Contract size limit** — AVM programs cap at 8 KB. Contracts that exceed this are split into helper subroutines via `--split-contracts`; see `src/splitter/`.
- **Inheritance is flattened** — Solidity's C3 linearization is collapsed at compile time so the emitted contract has all base methods inlined under their MRO names; no runtime delegatecall.
- **No try/catch** — AVM has no analogue for EVM revert-bubbling, so the entire `tryCatch/` semantic-test cluster (20 tests) is currently unsupported.
- **No CREATE2** — Salted deploys (`new C{salt: …}(…)`) have no AVM analogue (app IDs are assigned by the protocol at create time, not derived from salt+initcode hash) and the entire `saltedCreate/` cluster is unsupported. Plain `new C(...)` works via inner-txn app-create.
- **Delegate calls are WIP** — `address(L).delegatecall(...)` is currently stubbed at the call site (returns `(true, "")` with a warning) — there's no AVM equivalent of "execute foreign code in my storage context", so a faithful translation would require detecting the delegatecall pattern at compile time and inlining the target's body into the caller. The library-attached form (`using L for *`) works because it's resolved at compile time as a regular subroutine call; only the runtime `address(...).delegatecall(bytes)` form is stubbed.
- **Tokens compile to apps, not ASAs (for now)** — ERC20/721/1155 contracts are translated faithfully into AVM smart-contract apps with their own balance maps and transfer logic, the same way they live on EVM. This makes the upstream tests round-trip cleanly but ignores Algorand's biggest token-related feature: **ASAs** (Algorand Standard Assets) are first-class tokens at the protocol level, so things like balance lookups, transfers, freeze/clawback, and opt-in flows are all single opcodes / inner-txn fields rather than app calls. A future version will have native ERC20/721/1155 support and lower them onto an ASA created by the constructor; `transfer`/`balanceOf`/etc. become inner asset transfers and `acct_params_get AcctAssetBalance` reads, which is cheaper, composes natively with wallets and DEXes, and gets the security/UX properties of native assets for free. The smart-contract path stays as the fallback for tokens that need behavior ASAs don't expose (e.g. arbitrary `_beforeTokenTransfer` hooks, custom voting/snapshot logic).

## Related docs

- [`tests/solidity-semantic-tests/CURRENT.md`](tests/solidity-semantic-tests/CURRENT.md): living per-version progress log of the semantic testsuite.
