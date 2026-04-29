# puya-sol

Solidity → AVM (Algorand) compiler. Translates unmodified `.sol` source through Solidity's frontend to AWST (Algorand's WebAssembly-shaped IR), then hands off to [`puya`](https://github.com/algorandfoundation/puya) for AWST → TEAL lowering.

The pipeline:

```
.sol  ──[ puya-sol ]──▶  AWST JSON  ──[ puya ]──▶  TEAL + ARC-56
```

## Status

**1083 / 1322 (82%)** Solidity semantic tests passing as of v177. See [`tests/solidity-semantic-tests/CURRENT.md`](tests/solidity-semantic-tests/CURRENT.md) for the running changelog.

Real-world ports compiling and running on AVM localnet (under [`WIP/examples/`](WIP/examples/)):

- **Uniswap V2** (full AMM) and **V4** (361/411 tests passing)
- **OpenZeppelin** v5.0.0 — ERC20/721/1155, AccessControl, Ownable, Pausable, ReentrancyGuard, governance, vesting, and ~140 contracts in total
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

- **AWST is the contract** — puya-sol's job is to emit a well-typed AWST JSON that puya accepts. Test failures often come down to the wrong AWST shape rather than wrong semantics; the AWST round-trip is the primary debugging surface.
- **Storage maps to box state** — Solidity mappings/arrays/structs live in AVM **boxes** (one box per top-level state var, with sha256-derived keys for mapping entries). See `src/builder/storage/StorageMapper.cpp`.
- **Memory is a scratch-slot blob** — EVM's `memory` model is simulated via a 4096-byte byte-blob in scratch slot 0; `mload` / `mstore` lower to `extract3` / `replace3` against that blob. See `src/builder/assembly/MemoryHelpers.cpp`.
- **Contract size limit** — AVM programs cap at 8 KB. Contracts that exceed this are split into helper subroutines via `--split-contracts`; see `src/splitter/`.
- **Inheritance is flattened** — Solidity's C3 linearization is collapsed at compile time so the emitted contract has all base methods inlined under their MRO names; no runtime delegatecall.
- **No try/catch** — AVM has no analogue for EVM revert-bubbling, so the entire `tryCatch/` semantic-test cluster (20 tests) is currently unsupported.

## Related docs

- [`tests/solidity-semantic-tests/CURRENT.md`](tests/solidity-semantic-tests/CURRENT.md) — living per-version progress log
